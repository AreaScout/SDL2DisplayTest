# SDL2DisplayTest

![SDL2DisplayTest](https://www.hardkernel.com/main/_Files/prdt/2018/201805/201805120009102637.jpg)

This is an OSD SDL2Display Test for the new OGST Gaming Console Kit from Hardkernel, it listens for a event from the right user button on
the console. The program will run as a deamon to easily add it on startup scripts. You can add your own image which is of type Bitmap RGB565 and also TureTypeFonts

![SDL2DisplayTest](https://www.areascout.at/screen1.png)
## Building
```
$ sudo apt-get install libsdl2-dev libsdl2-ttf-dev
$ git clone https://github.com/AreaScout/SDL2DisplayTest.git
$ ./build.sh
```
## Add to autostart

here is an example of how it could look like in your /etc/rc.local file
```
[ `/sbin/lsmod | grep -c spi_s3c64xx` -ge 1 ] && rmmod spi_s3c64xx
modprobe spi_s3c64xx force32b=1
modprobe fbtft_device name=hktft9340 busnum=1 rotate=270
su root -c '/bin/rm -f /dev/fb1'
su root -c '/bin/mknod /dev/fb1 c 29 1'
su root -c '/bin/chmod 0666 /dev/fb1'
su root -c '/path/to/the/binary/OGSTDisplay'
su odroid -c 'cd /path/to/the/binary && ./OGSTDisplay odroid.bmp DroidSans-Bold.ttf'

exit 0
```
## Usage
first run it as root, the gpio device sysfs entry has to be created
```
sudo OGSTDISPLAY
./OGSTDisplay some.bmp some.ttf
```

Visit https://forum.odroid.com/viewtopic.php?f=156&t=30979
