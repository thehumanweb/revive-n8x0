#!/bin/sh
# Build n810-loader: loader/init.c (no libc) in a built-in initramfs, on a
# kernel from allnoconfig plus config/loader-6.18, in build/obj-loader-$V
# (the 6.18 tree as scripts/prepare-6.18.sh left it, patches and all) ->
# out/zImage-loader (zImage with the N810 DTB appended, for the kernel
# partition). The real kernel goes to the rootfs as /boot/zImage.
set -eu
cd "$(dirname "$0")/.."
V=6.18.58
K=build/linux-$V O=build/obj-loader-$V L=build/loader
BR=${BR_OUT:-../rootfs/build/br-out-armel}
CC=/br/host/bin/arm-buildroot-linux-gnueabi-gcc
mkdir -p $L $O out
# /init: static, no libc, ARMv6 EABI
docker run --rm -v "$PWD":/work -v "$(cd "$BR" && pwd)":/br:ro n810-buildroot sh -c "cd /work &&
	$CC -Os -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
		-march=armv6 -marm -Wall -Wextra -Wl,-e,_start -Wl,--gc-sections \
		-o $L/init loader/init.c -lgcc &&
	${CC%gcc}strip $L/init"
cat > $L/initramfs.list <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /r 0755 0 0
file /init /work/$L/init 0755 0 0
EOF
docker run --rm -v "$PWD":/work n810-kbuild:6 sh -c "cd /work/$K &&
	make O=/work/$O ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- \
		KCONFIG_ALLCONFIG=/work/config/loader-6.18 allnoconfig > /work/$L/build.log 2>&1 &&
	make O=/work/$O ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j16 \
		zImage ti/omap/omap2420-n810.dtb >> /work/$L/build.log 2>&1" ||
	{ grep -n -i "error" $L/build.log | tail -8; exit 1; }
# what the fragment asked for and the config didn't keep (a dependency missing)
grep '^CONFIG_.*=' config/loader-6.18 | while read -r l; do
	grep -qx "$l" $O/.config || echo "loader config: $l not kept"
done
cat $O/arch/arm/boot/zImage $O/arch/arm/boot/dts/ti/omap/omap2420-n810.dtb > out/zImage-loader
echo "out/zImage-loader: $(stat -c %s out/zImage-loader) bytes, init $(stat -c %s $L/init) bytes"
