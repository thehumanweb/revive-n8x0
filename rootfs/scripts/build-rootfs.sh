#!/bin/sh
# Build the N810 rootfs with Buildroot (docker image n810-buildroot, from
# ../kernel/docker/buildroot): out/rootfs.tar, rootfs.ext4 and rootfs.jffs2,
# with the kernel's modules overlaid (scripts/stage-modules.sh), the test programs
# (stage-tools.sh) and Mesa with the mbx driver built from ../mesa (local.mk).
# With DEVICE=1 the image is the one for the real N810 (out/rootfs-device.jffs2): the device
# overlay (board/n810-device: the stock initfs's contract, USB networking, clock), root
# password n810, busybox with udhcpd and ntpd, and /boot from build/boot-overlay
# (scripts/stage-boot.sh). Same Buildroot output tree: only the overlay and busybox differ.
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
DEFCONFIG=n810_defconfig
if [ "${DEVICE:-0}" = 1 ]; then
	scripts/stage-boot.sh
	sed -e 's|^BR2_TARGET_GENERIC_ROOT_PASSWD=.*|BR2_TARGET_GENERIC_ROOT_PASSWD="n810"|' \
	    -e 's|^BR2_ROOTFS_OVERLAY="\(.*\)"|BR2_ROOTFS_OVERLAY="\1 $(BR2_EXTERNAL_N810_PATH)/board/n810-device/overlay $(BR2_EXTERNAL_N810_PATH)/build/boot-overlay"|' \
	    configs/n810_defconfig > build/n810_device_defconfig
	echo 'BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_N810_PATH)/board/n810-device/busybox.fragment"' >> build/n810_device_defconfig
	echo 'BR2_ROOTFS_DEVICE_TABLE="system/device_table.txt $(BR2_EXTERNAL_N810_PATH)/board/n810-device/device_table.txt"' >> build/n810_device_defconfig
	DEFCONFIG=defconfig
fi
mkdir -p build/dl out
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work n810-buildroot sh -c "cd /work &&
	make -C build/buildroot-$V O=/work/build/br-out-armel BR2_EXTERNAL=/work BR2_DL_DIR=/work/build/dl \$( [ $DEFCONFIG = defconfig ] && echo BR2_DEFCONFIG=/work/build/n810_device_defconfig) $DEFCONFIG &&
	make -C build/br-out-armel BR2_JLEVEL=\$(nproc)" > build/build-rootfs.log 2>&1 ||
	{ tail -30 build/build-rootfs.log; exit 1; }
if [ "${DEVICE:-0}" = 1 ]; then
	cp build/br-out-armel/images/rootfs.jffs2 out/rootfs-device.jffs2
	# NOLO/0xFFFF treat a JFFS2 image under 16 MiB as an initfs: pad with erased flash to 17 MiB
	# (a whole number of 128 KiB erase blocks)
	sz=$(stat -c %s out/rootfs-device.jffs2)
	if [ "$sz" -lt $((17 * 1024 * 1024)) ]; then
		python3 -c "import sys; f=open(sys.argv[1],'ab'); f.write(b'\xff'*(17*1024*1024-int(sys.argv[2])))" out/rootfs-device.jffs2 "$sz"
	fi
	ls -l out; exit 0
fi
for f in rootfs.tar rootfs.ext4 rootfs.jffs2; do cp build/br-out-armel/images/$f out/; done
ls -l out
