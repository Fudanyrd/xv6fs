#!/usr/bin/env bash

qemu-system-aarch64 -M raspi3b \
    -dtb $LINUX/arch/arm64/boot/dts/broadcom/bcm2837-rpi-3-b.dtb \
    -kernel $LINUX/arch/arm64/boot/Image \
    -append "console=ttyAMA0,115200 earlycon=pl010 earlyprintk init=/init" \
    -initrd initrd.img \
    -usb -usbdevice "keyboard" \
    -serial null -serial mon:stdio -nographic

