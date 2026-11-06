#!/bin/sh
# Stage the kernel's modules and mbxaccess.ko as a rootfs overlay in
# build/modules-overlay (the defconfig lists it): the modules from
# ../kernel/scripts/build-6.18.sh, mbxaccess.ko from build-mbx.sh in
# lib/modules/<version>/extra, and a fresh depmod.
set -eu
cd "$(dirname "$0")/.."
V=6.18.58
K=../kernel
[ -d $K/build/modules-6.18/lib/modules/$V ] || { echo "stage-modules: run kernel/scripts/build-6.18.sh first" >&2; exit 1; }
[ -f $K/out/mbxaccess.ko ] || { echo "stage-modules: run kernel/scripts/build-mbx.sh first" >&2; exit 1; }
rm -rf build/modules-overlay
mkdir -p build/modules-overlay/lib/modules
cp -a $K/build/modules-6.18/lib/modules/$V build/modules-overlay/lib/modules/
M=build/modules-overlay/lib/modules/$V
rm -f $M/build $M/source
mkdir -p $M/extra
cp $K/out/mbxaccess.ko $M/extra/
docker run --rm -v "$PWD":/work n810-kbuild:6 depmod -b /work/build/modules-overlay $V
echo "staged $(find $M -name '*.ko' | wc -l) modules in build/modules-overlay"
