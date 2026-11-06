#!/bin/sh
# Cross-build the MBX test programs with Buildroot's toolchain (build-rootfs.sh
# has made it) into build/tools-overlay/usr/bin, a rootfs overlay.
set -eu
cd "$(dirname "$0")/.."
H=build/br-out-armel/host
CC=$PWD/$H/bin/arm-buildroot-linux-gnueabi-gcc
SYS=$H/arm-buildroot-linux-gnueabi/sysroot
[ -x $CC ] || { echo "stage-tools: no toolchain at $CC (run build-rootfs.sh once)" >&2; exit 1; }
rm -rf build/tools-overlay
mkdir -p build/tools-overlay/usr/bin
make -C ../mbx mbxtri-drm CC=$CC CFLAGS="-O2 -std=gnu11 -Wall" EXTRA="-I$PWD/$SYS/usr/include/drm" >/dev/null
mv ../mbx/mbxtri-drm build/tools-overlay/usr/bin/
echo "staged $(ls build/tools-overlay/usr/bin | tr '\n' ' ')"
