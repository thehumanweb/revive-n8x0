#!/bin/sh
# Build the N810 rootfs with Buildroot (docker image n810-buildroot, from
# ../kernel/docker/buildroot): out/rootfs.tar, rootfs.ext4 and rootfs.jffs2,
# with the kernel's modules overlaid (scripts/stage-modules.sh), the test programs
# (stage-tools.sh) and Mesa with the mbx driver built from ../mesa (local.mk).
# Buildroot's output tree is build/br-out-armel (its toolchain is also what
# ../kernel/scripts/build-loader.sh compiles the loader with).
set -eu
cd "$(dirname "$0")/.."
V=2026.08.1
scripts/fetch-buildroot.sh
[ -d build/buildroot-$V ] || { mkdir -p build && tar -xJf src/buildroot-$V.tar.xz -C build; }
../mesa/scripts/prepare-mesa.sh
# the build container only sees rootfs/: copy what it builds from (Mesa's prepared
# tree for local.mk, the demo sources for package/mbx-demo) under build/
mkdir -p build/mesa-src build/mbx-demo-src
cp -a -u ../build/mesa/mesa-26.1.8/. build/mesa-src/
cp -a -u ../mesa/tests/. build/mbx-demo-src/
# Buildroot syncs an override source dir only when its sync stamp is missing: clear
# the stamps of the packages built from our trees, so each run re-syncs what changed
# (rsync keeps unchanged files and times), reconfigures and rebuilds incrementally
rm -f build/br-out-armel/build/mesa3d-custom/.stamp_* build/br-out-armel/build/mbx-demo-*/.stamp_*
scripts/stage-modules.sh
scripts/stage-tools.sh
mkdir -p build/dl out
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work n810-buildroot sh -c "cd /work &&
	make -C build/buildroot-$V O=/work/build/br-out-armel BR2_EXTERNAL=/work BR2_DL_DIR=/work/build/dl n810_defconfig &&
	make -C build/br-out-armel BR2_JLEVEL=\$(nproc)" > build/build-rootfs.log 2>&1 ||
	{ tail -30 build/build-rootfs.log; exit 1; }
for f in rootfs.tar rootfs.ext4 rootfs.jffs2; do cp build/br-out-armel/images/$f out/; done
ls -l out
