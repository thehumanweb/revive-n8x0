#!/bin/sh
# Run on the jig host (storage), in ~/n810/mbx-test: flash our rootfs image (the
# MBX test system) into the N810's rootfs partition. The kernel partition (the kexec
# loader), the initfs, the bootloader and the CAL are not touched; the loader boots
# /boot/zImage from the new rootfs, with /boot/zImage.old (the kernel the unit ran
# before) as its fallback. Start this, then reboot the device so NOLO takes the image
# over USB (Diablo's power menu, or off and on). Unplug the charger first unless
# charging under our stack is meant.
set -e
cd "$(dirname "$0")"
[ -f rootfs-device.jffs2 ] || { echo "missing rootfs-device.jffs2"; exit 1; }
echo "waiting for the N810 in NOLO (reboot it now)..."
0xFFFF -m rootfs:rootfs-device.jffs2 -f -r
