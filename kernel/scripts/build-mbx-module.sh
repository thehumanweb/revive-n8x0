#!/bin/sh
# Fast loop for the MBX DRM driver (../mbx/drm): copy it into the prepared
# 6.18 tree, build just that module, and install it into build/modules-6.18
# (where rootfs/scripts/stage-modules.sh takes it from). Run build-6.18.sh
# once first. The tree is not re-extracted, so this is a dev shortcut:
# prepare-6.18.sh does the same copy and is what a clean build uses.
set -eu
cd "$(dirname "$0")/.."
V=6.18.58
K=build/linux-$V O=build/obj-$V
cp ../mbx/drm/Kconfig ../mbx/drm/Makefile ../mbx/drm/*.c ../mbx/drm/mbx_drv.h ../mbx/drm/mbx_regs.h $K/drivers/gpu/drm/mbx/
cp ../mbx/drm/mbx_drm.h $K/include/uapi/drm/mbx_drm.h
docker run --rm -v "$PWD":/work n810-kbuild:6 sh -c "cd $K &&
	make O=/work/$O ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- drivers/gpu/drm/mbx/ modules" > build/build-mbx-module.log 2>&1 ||
	{ grep -n -E "error|warning" build/build-mbx-module.log | head -40; exit 1; }
grep -E "warning" build/build-mbx-module.log | head -20 || true
D=build/modules-6.18/lib/modules/$V/kernel/drivers/gpu/drm/mbx
mkdir -p $D
docker run --rm -v "$PWD":/work n810-kbuild:6 arm-linux-gnueabi-strip --strip-debug -o /work/$D/mbx.ko /work/$O/drivers/gpu/drm/mbx/mbx.ko
echo "$D/mbx.ko: $(stat -c %s $D/mbx.ko) bytes"
