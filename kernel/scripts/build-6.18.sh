#!/bin/sh
# Build the prepared 6.18 tree (incrementally, objects in build/obj-$V) and
# make out/zImage-6.18: zImage with the N810 DTB appended (NOLO and QEMU pass
# ATAGs, not a DTB), and out/zImage-6.18-n800 with the N800 DTB. Modules go
# to build/modules-6.18 for the rootfs.
set -eu
cd "$(dirname "$0")/.."
V=6.18.58
K=build/linux-$V O=build/obj-$V
docker run --rm -v "$PWD":/work n810-kbuild:6 sh -c "cd $K &&
	make O=/work/$O ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j16 zImage modules ti/omap/omap2420-n810.dtb ti/omap/omap2420-n800.dtb > /work/build/build-6.18.log 2>&1 &&
	rm -rf /work/build/modules-6.18 &&
	make O=/work/$O ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- INSTALL_MOD_PATH=/work/build/modules-6.18 INSTALL_MOD_STRIP=1 modules_install >> /work/build/build-6.18.log 2>&1" ||
	{ grep -n "error" build/build-6.18.log | tail -8; exit 1; }
mkdir -p out
cat $O/arch/arm/boot/zImage $O/arch/arm/boot/dts/ti/omap/omap2420-n810.dtb > out/zImage-6.18
cat $O/arch/arm/boot/zImage $O/arch/arm/boot/dts/ti/omap/omap2420-n800.dtb > out/zImage-6.18-n800
echo "out/zImage-6.18: $(stat -c %s out/zImage-6.18) bytes, $(grep -c ' CC ' build/build-6.18.log) files compiled, $(grep -c 'warning:' build/build-6.18.log) warnings"
# NOLO loads the kernel from the 0x200000-byte kernel partition (its
# partition tags on a real N810)
size=$(stat -c %s out/zImage-6.18) max=$((0x200000))
if [ $size -gt $max ]; then
	echo "WARNING: $((size - max)) bytes too big for the kernel partition ($max); QEMU only"
else
	echo "fits the kernel partition: $((max - size)) bytes free"
fi
