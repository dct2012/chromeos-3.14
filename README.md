This is a fork of ChromiumOS's chromeos-3.14 kernel. It's aim is to provide a fully working kernel for a regular Linux install on the Chromebook Pixel 2.
Since this will also be my personal kernel I'm adding the bfs, uksm and greysky's gcc optimization patches.

To make a basic .config run 
    $ ./chromeos/scripts/prepareconfig chromeos-intel-pineview
And then run make menuconfig or whatever you use to select the things you like.

**Issues**
* Swithing to a tty from X or leaving X causes the backlight to turn off
    * I've been able to find a way to turn it back on, but it still turns off for a moment
* Audio
    * Speakers work, but alsamixer always see's volume at 0 even when there's audio playing at a volume that's hearable
    * Not sure if headphones work. Automatic switch to headphones definitly doesn't work

