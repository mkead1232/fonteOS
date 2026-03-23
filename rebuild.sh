#!/bin/sh
set -e
cd ~/fonteOS/boot-files/initramfs
find . | cpio -o -H newc > ../init.cpio
cd ~/fonteOS
cp boot-files/init.cpio boot-files/iso/boot/init.cpio
xorriso -as mkisofs \
  -b boot/limine/limine-bios-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot boot/limine/limine-uefi-cd.bin \
  -efi-boot-part --efi-boot-image \
  -o boot-files/fonteos.iso \
  boot-files/iso
limine/limine bios-install boot-files/fonteos.iso
echo "==> done"
