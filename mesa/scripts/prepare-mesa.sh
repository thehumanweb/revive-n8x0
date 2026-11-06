#!/bin/sh
# prepare-mesa.sh: Mesa 26.1.8 (the version Buildroot 2026.08.1 ships), checked
# against its published sha256, with the hook patches in mesa/patches applied
# and the mbx driver sources (mesa/src) and uapi header (mbx/drm/mbx_drm.h)
# copied in. Result: build/mesa/mesa-26.1.8.
set -eu
cd "$(dirname "$0")/../.."
V=26.1.8
SUM=b320f65874fd9653ac6c0bd1616605387344e1247411a50c797b5f3fb9dc0b55
T=rootfs/build/dl/mesa-$V.tar.xz
mkdir -p rootfs/build/dl build/mesa
if [ ! -f $T ]; then
	curl -fL -o $T.part https://archive.mesa3d.org/mesa-$V.tar.xz && mv $T.part $T
fi
echo "$SUM  $T" | sha256sum -c - >/dev/null
rm -rf build/mesa/mesa-$V
tar -xJf $T -C build/mesa
M=build/mesa/mesa-$V
for p in mesa/patches/*.patch; do
	patch -d $M -p1 -s < $p
	echo "ok    $p"
done
cp -a mesa/src/. $M/src/
cp mbx/drm/mbx_drm.h $M/include/drm-uapi/mbx_drm.h
# the emulator core, for the host sim (drm-shim)
E=$M/src/gallium/drivers/mbx/drm-shim/emu
mkdir -p $E
cp mbx/core.c mbx/ta.c mbx/vgp.c mbx/render.c mbx/rcptab.c mbx/texture.c mbx/twod.c mbx/pb.c mbx/emu.h mbx/mbxemu.h $E/
echo "prepared $M"
