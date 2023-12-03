# Simple Block Device Driver
Implementation of Linux Kernel 4.18.X simple block device.

## Build
- regular:
`$ make`
- with requests debug info:
uncomment `CFLAGS_sbdd.o := -DDEBUG` in `Kbuild`

## Create a loop device for testing:
- dd if=/dev/urandom of=disk_file bs=1024 count=100000
- losetup /dev/loop0 file_dev
- blockdev --getsize /dev/loop0

## Initialize and stack the device
- insmod sbdd.ko
- ./util/sbdd_util /dev/loop0
- blockdev --getsize /dev/sbdd0

## Mount the device and use it
- mkdir ~/mnt
- mkfs.ext4 /dev/sbdd0
- mount -t ext4 /dev/sbdd0 ~/mnt/

## Unmount and remove device
- umount /dev/sbdd0
- rmmod stackbd

## Clean
`$ make clean`

## References
- [Linux Device Drivers](https://lwn.net/Kernel/LDD3/)
- [Linux Kernel Development](https://rlove.org)
- [Linux Kernel Teaching](https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html)
- [Linux Kernel Sources](https://github.com/torvalds/linux)
