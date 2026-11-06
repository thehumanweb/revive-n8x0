#!/bin/sh
# Build mbxaccess.ko (the PowerVR MBX access driver, ../mbx/kmod) against our
# 6.18 kernel (run scripts/build-6.18.sh first). The sources are copied to
# build/kmod-src so the build leaves nothing in the source tree.
set -eu
cd "$(dirname "$0")/.."
V=6.18.58
rm -rf build/kmod-src
mkdir -p build out
cp -a ../mbx/kmod build/kmod-src
docker run --rm -v "$PWD":/work n810-kbuild:6 sh -c "cd /work/build/linux-$V &&
	make O=/work/build/obj-$V ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- M=/work/build/kmod-src -k modules" \
	> build/build-mbx.log 2>&1 || { grep -n -E "error|Error" build/build-mbx.log | head -40; exit 1; }
cp build/kmod-src/mbxaccess.ko out/mbxaccess.ko
echo "out/mbxaccess.ko: $(stat -c %s out/mbxaccess.ko) bytes, $(grep -c "warning:" build/build-mbx.log) warnings"
