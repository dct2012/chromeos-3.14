/*
 * Copyright (C) 2011 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@android.com>
 *
 * Copyright (C) 2010,2013, NVIDIA Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/cpu_pm.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip/arm-gic.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/syscore_ops.h>

#include <soc/tegra/fuse.h>
#include <soc/tegra/iomap.h>

#include "irqchip.h"

#define ICTLR_CPU_IEP_VFIQ	0x08
#define ICTLR_CPU_IEP_FIR	0x14
#define ICTLR_CPU_IEP_FIR_SET	0x18
#define ICTLR_CPU_IEP_FIR_CLR	0x1c

#define ICTLR_CPU_IER		0x20
#define ICTLR_CPU_IER_SET	0x24
#define ICTLR_CPU_IER_CLR	0x28
#define ICTLR_CPU_IEP_CLASS	0x2C

#define ICTLR_COP_IER		0x30
#define ICTLR_COP_IER_SET	0x34
#define ICTLR_COP_IER_CLR	0x38
#define ICTLR_COP_IEP_CLASS	0x3c

#define FIRST_LEGACY_IRQ 32
#define TEGRA_MAX_NUM_ICTLRS	5

#define SGI_MASK 0xFFFF

static int num_ictlrs;

static void __iomem *ictlr_reg_base[] = { NULL, NULL, NULL, NULL, NULL };

#ifdef CONFIG_PM_SLEEP
static u32 cop_ier[TEGRA_MAX_NUM_ICTLRS];
static u32 cop_iep[TEGRA_MAX_NUM_ICTLRS];
static u32 cpu_ier[TEGRA_MAX_NUM_ICTLRS];
static u32 cpu_iep[TEGRA_MAX_NUM_ICTLRS];

static u32 ictlr_wake_mask[TEGRA_MAX_NUM_ICTLRS];
static void __iomem *tegra_gic_cpu_base;
#endif

static void __iomem *distbase;

bool tegra_pending_sgi(void)
{
	u32 pending_set;

	pending_set = readl_relaxed(distbase + GIC_DIST_PENDING_SET);

	if (pending_set & SGI_MASK)
		return true;

	return false;
}

static inline void tegra_irq_write_mask(unsigned int irq, unsigned long reg)
{
	void __iomem *base;
	u32 mask;

	BUG_ON(irq < FIRST_LEGACY_IRQ ||
		irq >= FIRST_LEGACY_IRQ + num_ictlrs * 32);

	base = ictlr_reg_base[(irq - FIRST_LEGACY_IRQ) / 32];
	mask = BIT((irq - FIRST_LEGACY_IRQ) % 32);

	__raw_writel(mask, base + reg);
}

static void tegra_mask(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IER_CLR);
}

static void tegra_unmask(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IER_SET);
}

static void tegra_ack(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IEP_FIR_CLR);
}

static void tegra_eoi(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IEP_FIR_CLR);
}

static int tegra_retrigger(struct irq_data *d)
{
	if (d->irq < FIRST_LEGACY_IRQ)
		return 0;

	tegra_irq_write_mask(d->irq, ICTLR_CPU_IEP_FIR_SET);

	return 1;
}

#ifdef CONFIG_PM_SLEEP
static int tegra_set_wake(struct irq_data *d, unsigned int enable)
{
	u32 irq = d->irq;
	u32 index, mask;

	if (irq < FIRST_LEGACY_IRQ ||
		irq >= FIRST_LEGACY_IRQ + num_ictlrs * 32)
		return -EINVAL;

	index = ((irq - FIRST_LEGACY_IRQ) / 32);
	mask = BIT((irq - FIRST_LEGACY_IRQ) % 32);
	if (enable)
		ictlr_wake_mask[index] |= mask;
	else
		ictlr_wake_mask[index] &= ~mask;

	return 0;
}

static int tegra_legacy_irq_suspend(void)
{
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for (i = 0; i < num_ictlrs; i++) {
		void __iomem *ictlr = ictlr_reg_base[i];
		/* Save interrupt state */
		cpu_ier[i] = readl_relaxed(ictlr + ICTLR_CPU_IER);
		cpu_iep[i] = readl_relaxed(ictlr + ICTLR_CPU_IEP_CLASS);
		cop_ier[i] = readl_relaxed(ictlr + ICTLR_COP_IER);
		cop_iep[i] = readl_relaxed(ictlr + ICTLR_COP_IEP_CLASS);

		/* Disable COP interrupts */
		writel_relaxed(~0ul, ictlr + ICTLR_COP_IER_CLR);

		/* Disable CPU interrupts */
		writel_relaxed(~0ul, ictlr + ICTLR_CPU_IER_CLR);

		/* Enable the wakeup sources of ictlr */
		writel_relaxed(ictlr_wake_mask[i], ictlr + ICTLR_CPU_IER_SET);
	}
	local_irq_restore(flags);

	return 0;
}

static void tegra_legacy_irq_resume(void)
{
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for (i = 0; i < num_ictlrs; i++) {
		void __iomem *ictlr = ictlr_reg_base[i];
		writel_relaxed(cpu_iep[i], ictlr + ICTLR_CPU_IEP_CLASS);
		writel_relaxed(~0ul, ictlr + ICTLR_CPU_IER_CLR);
		writel_relaxed(cpu_ier[i], ictlr + ICTLR_CPU_IER_SET);
		writel_relaxed(cop_iep[i], ictlr + ICTLR_COP_IEP_CLASS);
		writel_relaxed(~0ul, ictlr + ICTLR_COP_IER_CLR);
		writel_relaxed(cop_ier[i], ictlr + ICTLR_COP_IER_SET);
	}
	local_irq_restore(flags);
}

static struct syscore_ops tegra_legacy_irq_syscore_ops = {
	.suspend = tegra_legacy_irq_suspend,
	.resume = tegra_legacy_irq_resume,
};

int tegra_legacy_irq_syscore_init(void)
{
	register_syscore_ops(&tegra_legacy_irq_syscore_ops);

	return 0;
}

static int tegra_gic_notifier(struct notifier_block *self,
			      unsigned long cmd, void *v)
{
	switch (cmd) {
	case CPU_PM_ENTER:
		writel_relaxed(0x1E0, tegra_gic_cpu_base + GIC_CPU_CTRL);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block tegra_gic_notifier_block = {
	.notifier_call = tegra_gic_notifier,
};

static const struct of_device_id tegra114_dt_gic_match[] __initconst = {
	{ .compatible = "arm,cortex-a15-gic" },
	{ }
};

static void tegra114_gic_cpu_pm_registration(void)
{
	struct device_node *dn;

	dn = of_find_matching_node(NULL, tegra114_dt_gic_match);
	if (!dn)
		return;

	tegra_gic_cpu_base = of_iomap(dn, 1);

	cpu_pm_register_notifier(&tegra_gic_notifier_block);
}
#else
#define tegra_set_wake NULL
static void tegra114_gic_cpu_pm_registration(void) { }
#endif

static struct resource ictlr_regs[] = {
	{ .start = 0x60004000, .end = 0x6000403f, .flags = IORESOURCE_MEM },
	{ .start = 0x60004100, .end = 0x6000413f, .flags = IORESOURCE_MEM },
	{ .start = 0x60004200, .end = 0x6000423f, .flags = IORESOURCE_MEM },
	{ .start = 0x60004300, .end = 0x6000433f, .flags = IORESOURCE_MEM },
	{ .start = 0x60004400, .end = 0x6000443f, .flags = IORESOURCE_MEM },
};

struct tegra_ictlr_soc {
	unsigned int num_ictlrs;
};

static const struct tegra_ictlr_soc tegra20_ictlr_soc = {
	.num_ictlrs = 4,
};

static const struct tegra_ictlr_soc tegra30_ictlr_soc = {
	.num_ictlrs = 5,
};

static const struct of_device_id ictlr_matches[] = {
	{ .compatible = "nvidia,tegra30-ictlr", .data = &tegra30_ictlr_soc },
	{ .compatible = "nvidia,tegra20-ictlr", .data = &tegra20_ictlr_soc },
	{ }
};

static const struct of_device_id gic_matches[] = {
	{ .compatible = "arm,cortex-a15-gic", },
	{ .compatible = "arm,cortex-a9-gic", },
	{ }
};

static int __init tegra_irq_init(struct device_node *np,
				 struct device_node *parent)
{
	unsigned int max_ictlrs = ARRAY_SIZE(ictlr_regs), i;
	const struct of_device_id *match;
	struct device_node *gic;
	struct resource res;

	if (np) {
		const struct tegra_ictlr_soc *soc;

		match = of_match_node(ictlr_matches, np);
		soc = match->data;

		for (i = 0; i < soc->num_ictlrs; i++) {
			if (of_address_to_resource(np, i, &res) < 0)
				break;

			ictlr_regs[i] = res;
		}

		WARN(i != soc->num_ictlrs,
		     "Found %u interrupt controllers in DT; expected %u.\n",
		     i, soc->num_ictlrs);

		max_ictlrs = soc->num_ictlrs;
	} else {
		/*
		 * If no matching device node was found, fall back to using
		 * the chip ID.
		 */

		/* Tegra30 and later have five interrupt controllers, ... */
		max_ictlrs = ARRAY_SIZE(ictlr_regs);

		/* ..., but Tegra20 only has four. */
		if (of_machine_is_compatible("nvidia,tegra20"))
			max_ictlrs--;
	}

	memset(&res, 0, sizeof(res));

	gic = of_find_matching_node(NULL, gic_matches);
	if (gic) {
		if (of_address_to_resource(gic, 0, &res) < 0)
			WARN(1, "GIC registers are missing from DT\n");

		of_node_put(gic);
	}

	if (res.start == 0 || res.end == 0) {
		res.start = 0x50041000;
		res.end = 0x50041fff;
		res.flags = IORESOURCE_MEM;
	}

	distbase = ioremap_nocache(res.start, resource_size(&res));
	num_ictlrs = readl_relaxed(distbase + GIC_DIST_CTR) & 0x1f;

#ifdef CONFIG_ARCH_TEGRA_132_SOC
	/*
	 * On Tegra 132 there are technically 6 controllers, but
	 * one of them needs to be special cased and so we should
	 * ignore it here.
	 */
	num_ictlrs--;
#endif

	if (num_ictlrs != max_ictlrs) {
		WARN(1, "Found %u interrupt controllers; expected %u.\n",
		     num_ictlrs, max_ictlrs);
		num_ictlrs = max_ictlrs;
	}

	for (i = 0; i < num_ictlrs; i++) {
		struct resource *regs = &ictlr_regs[i];
		void __iomem *ictlr;

		ictlr = ioremap_nocache(regs->start, resource_size(regs));
		writel(~0, ictlr + ICTLR_CPU_IER_CLR);
		writel(0, ictlr + ICTLR_CPU_IEP_CLASS);
		ictlr_reg_base[i] = ictlr;
	}

	gic_arch_extn.irq_ack = tegra_ack;
	gic_arch_extn.irq_eoi = tegra_eoi;
	gic_arch_extn.irq_mask = tegra_mask;
	gic_arch_extn.irq_unmask = tegra_unmask;
	gic_arch_extn.irq_retrigger = tegra_retrigger;
	gic_arch_extn.irq_set_wake = tegra_set_wake;
	gic_arch_extn.flags = IRQCHIP_MASK_ON_SUSPEND;

	tegra114_gic_cpu_pm_registration();

	pr_info("Tegra Legacy Interrupt Controller initialized.\n");

	return 0;
}
#ifdef CONFIG_ARCH_TEGRA_132_SOC
IRQCHIP_DECLARE(tegra30_irq_chip, "nvidia,tegra30-ictlr", tegra_irq_init);
IRQCHIP_DECLARE(tegra20_irq_chip, "nvidia,tegra20-ictlr", tegra_irq_init);
#else
void __init tegra_init_irq(void)
{
	const struct of_device_id *match;
	struct device_node *np, *parent;

	np = of_find_matching_node_and_match(NULL, ictlr_matches, &match);
	if (!np) {
		pr_err("Can't find matching ictrl node\n");
		return;
	}

	parent = of_get_parent(np);
	if (!parent) {
		pr_err("Can't get ictrl parent\n");
		of_node_put(np);
		return;
	};

	tegra_irq_init(np, parent);

	of_node_put(parent);
	of_node_put(np);
}
#endif
