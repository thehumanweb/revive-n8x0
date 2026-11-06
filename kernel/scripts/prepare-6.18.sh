#!/bin/sh
# Prepare an N810 Linux 6.18 tree: kernel.org tarball plus patches/6.18,
# config = omap2plus_defconfig merged with config/local-6.18, then
# config/size-6.18 (fit NOLO's kernel partition).
#
# Builds are incremental: objects live in build/obj-$V (make O=), the source
# tree is re-extracted each time with the tarball's timestamps, so only files
# our patches touch look new. Files touched by the previous or the current
# patch set are touched again, so a dropped or changed patch can't leave a
# stale object behind.
# Usage: scripts/prepare-6.18.sh [EXTRA_FRAGMENT]
#   (writes build/linux-$V and build/obj-$V/.config; an extra config fragment,
#   e.g. config/test-fbcon-6.18 or config/debug-6.18, is merged last)
set -eu
cd "$(dirname "$0")/.."
V=6.18.58
K=build/linux-$V O=build/obj-$V LIST=build/patched-files-$V.txt
mkdir -p build $O
touch $LIST
rm -rf $K
tar -xJf src/linux-$V.tar.xz -C build
for p in $(ls patches/6.18/*.patch 2>/dev/null | sort); do
	patch -d $K -p1 -s --no-backup-if-mismatch < $p
	echo "ok    $p"
done
# The MBX DRM driver lives in ../mbx/drm (hooked in by patch 0063)
mkdir -p $K/drivers/gpu/drm/mbx
cp ../mbx/drm/Kconfig ../mbx/drm/Makefile ../mbx/drm/*.c ../mbx/drm/mbx_drv.h ../mbx/drm/mbx_regs.h $K/drivers/gpu/drm/mbx/
cp ../mbx/drm/mbx_drm.h $K/include/uapi/drm/mbx_drm.h
sed -n 's#^+++ b/\([^	 ]*\).*#\1#p' patches/6.18/*.patch | sort -u > $LIST.new
sort -u $LIST $LIST.new | while read -r f; do
	[ -e "$K/$f" ] && touch "$K/$f"
done
mv $LIST.new $LIST
docker run --rm -v "$PWD":/work n810-kbuild:6 sh -c "cd $K &&
	make O=/work/$O ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- omap2plus_defconfig > /dev/null &&
	scripts/kconfig/merge_config.sh -m -O /work/$O /work/$O/.config /work/config/local-6.18 /work/config/size-6.18 ${1:+/work/$1} > /dev/null &&
	make O=/work/$O ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- olddefconfig > /dev/null"
# Report fragment lines that didn't stick
cat config/local-6.18 config/size-6.18 ${1:-} | grep '^\(CONFIG\|# CONFIG\)' | while read -r l; do
	grep -qx "$l" $O/.config || echo "not applied: $l"
done
echo "prepared $K (objects in $O)"
