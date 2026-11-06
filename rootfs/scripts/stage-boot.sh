#!/bin/sh
# Stage /boot for the device rootfs in build/boot-overlay: the kexec loader takes
# /boot/zImage (N810) or /boot/zImage-n800, then /boot/zImage.old. zImage and
# zImage-n800 are this repo's kernels (kernel/out, with the mbx DT node); zImage.old
# is the kernel the unit runs now, a known-good one, copied from the device
# beforehand into build/device-zImage.old (see scripts/fetch-device-kernel.sh).
set -eu
cd "$(dirname "$0")/.."
K=../kernel/out
[ -f $K/zImage-6.18 ] && [ -f $K/zImage-6.18-n800 ] || { echo "stage-boot: build the kernel first" >&2; exit 1; }
[ -f build/device-zImage.old ] || { echo "stage-boot: build/device-zImage.old missing (scripts/fetch-device-kernel.sh)" >&2; exit 1; }
rm -rf build/boot-overlay
mkdir -p build/boot-overlay/boot
cp $K/zImage-6.18 build/boot-overlay/boot/zImage
cp $K/zImage-6.18-n800 build/boot-overlay/boot/zImage-n800
cp build/device-zImage.old build/boot-overlay/boot/zImage.old
ls -l build/boot-overlay/boot
