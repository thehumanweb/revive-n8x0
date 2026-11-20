#!/bin/sh
# Copy the device rootfs image and the flash/restore helpers to the jig host's
# ~/n810/mbx-test (a new directory: nothing there is overwritten) and print the sha256
# sums on both ends. The N810 is not touched.
set -eu
cd "$(dirname "$0")/../.."
H=${JIG_HOST:?set JIG_HOST=user@jig-host}	# the jig host: user@host (JIG_HOST is required)
IMG=rootfs/out/rootfs-device.jffs2
[ -f $IMG ] || { echo "build it first: cd rootfs && DEVICE=1 scripts/build-rootfs.sh" >&2; exit 1; }
ssh -o BatchMode=yes $H 'mkdir -p ~/n810/mbx-test'
scp -q -o BatchMode=yes $IMG scripts/device/flash-mbx.sh scripts/device/restore-state.sh $H:n810/mbx-test/
sha256sum $IMG
ssh -o BatchMode=yes $H 'cd ~/n810/mbx-test && sha256sum rootfs-device.jffs2 && ls -l'
