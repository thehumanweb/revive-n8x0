#!/bin/sh
# Boot our device rootfs under the stock Maemo initfs in QEMU, the way the real unit does:
# the kernel mounts the initfs partition (mtd3, root=1f03), its linuxrc mounts the rootfs
# (mtd4), pivots into it and runs /sbin/init. Reproduces hand-over failures offline.
# Usage: scripts/qemu-initfs.sh INITFS_IMAGE [ROOTFS_IMAGE [SECONDS]]; the serial console
# is written to build/initfs-boot.log. The initfs image is the stock firmware part
# (N8X0_NOTES.md section 2a); it is not in this repo.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
INITFS=${1:?initfs image}; ROOTFS=${2:-$here/rootfs/out/rootfs-device.jffs2}; SECS=${3:-60}
QEMU=${QEMU:-$here/build/qemu-src/build/qemu-system-arm}
FLASH=$here/build/flash-initfs.img
python3 - "$FLASH" "$INITFS" "$ROOTFS" <<'P'
import sys
with open(sys.argv[1], "wb") as f:
    f.write(b"\xff" * ((256 << 20) + (8 << 20)))
    f.seek(0x280000)                   # "initfs" partition (mtd3)
    f.write(open(sys.argv[2], "rb").read())
    f.seek(0x680000)                   # "rootfs" partition (mtd4)
    f.write(open(sys.argv[3], "rb").read())
P
rm -f "$here/build/initfs-boot.log"
( "$QEMU" -machine n810 -kernel "$here/kernel/out/zImage-6.18" -mtdblock "$FLASH" \
	-append "console=ttyS2,115200n8 root=1f03 rootfstype=jffs2 ro" \
	-display none -serial null -serial null -serial file:"$here/build/initfs-boot.log" -monitor none & echo $! > "$here/build/initfs-qemu.pid" )
sleep "$SECS"
kill "$(cat "$here/build/initfs-qemu.pid")" 2>/dev/null || true
tr -d '\r' < "$here/build/initfs-boot.log" | tail -n ${TAIL:-40}
