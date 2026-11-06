# Buildroot's override file (BR2_PACKAGE_OVERRIDE_FILE): build Mesa from the tree
# mesa/scripts/prepare-mesa.sh prepares (Mesa 26.1.8 with the mbx patches and sources),
# copied to build/mesa-src by scripts/build-rootfs.sh (the build container sees rootfs/ only).
MESA3D_OVERRIDE_SRCDIR = $(BR2_EXTERNAL_N810_PATH)/build/mesa-src
