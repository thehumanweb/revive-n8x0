#!/bin/sh
# Copy the kernel the N810 runs now (/boot/zImage, read-only) to build/device-zImage.old,
# through the jig host (storage.netbird.cloud, ~/n810/n810-net.sh). It becomes the
# new rootfs's /boot/zImage.old, the loader's fallback.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
ssh -o BatchMode=yes carsten@storage.netbird.cloud 'cd ~/n810 && sh n810-net.sh "cat /boot/zImage"' > build/device-zImage.old.tmp
[ "$(stat -c %s build/device-zImage.old.tmp)" -gt 1000000 ] || { echo "short read" >&2; exit 1; }
mv build/device-zImage.old.tmp build/device-zImage.old
sha256sum build/device-zImage.old
ls -l build/device-zImage.old
