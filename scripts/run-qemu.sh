#!/bin/sh
# Boot the N810 in the patched QEMU (see NOTES.md): JFFS2 rootfs on OneNAND,
# console on the third UART at tcp:127.0.0.1:$PORT (log in as root, no password).
# Usage: scripts/run-qemu.sh [-k zImage] [-r rootfs.jffs2] [extra qemu args...]
#   QEMU=path/to/qemu-system-arm (default: build/qemu-src/qemu-9.1.3/build/, where README.md's "Build"
#   section puts it)  PORT=5577  FLASH=flash.img (rebuilt from the rootfs)
#   MONITOR=unix:build/mon.sock,server,nowait  gives QEMU a monitor (screendump)
#   MBX_TRACE=1|2  logs MBX register accesses (2: every TA and 2D word too)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU:-$here/build/qemu-src/qemu-9.1.3/build/qemu-system-arm}
PORT=${PORT:-5577}
KERNEL=$here/kernel/out/zImage-6.18
ROOTFS=$here/rootfs/out/rootfs.jffs2
FLASH=${FLASH:-$here/build/flash.img}
while getopts k:r: o; do
	case $o in k) KERNEL=$OPTARG;; r) ROOTFS=$OPTARG;; *) exit 2;; esac
done
shift $((OPTIND - 1))

[ -x "$QEMU" ] || { echo "run-qemu.sh: no QEMU at $QEMU; build it as README.md says (Build, step 1) or set QEMU=" >&2; exit 1; }
mkdir -p "$(dirname "$FLASH")"
if [ ! -e "$FLASH" ] || [ "$ROOTFS" -nt "$FLASH" ]; then
	# OneNAND keeps its spare area after the main data: 256 MB + 8 MB, all 0xff
	python3 - "$FLASH" "$ROOTFS" <<'P'
import sys
with open(sys.argv[1], "wb") as f:
    f.write(b"\xff" * ((256 << 20) + (8 << 20)))
    f.seek(0x680000)                   # "rootfs" partition (mtd4)
    f.write(open(sys.argv[2], "rb").read())
P
fi
exec "$QEMU" -machine n810 -kernel "$KERNEL" -mtdblock "$FLASH" \
	-append "console=ttyS2,115200 root=/dev/mtdblock4 rootfstype=jffs2 rw" \
	-display none -serial null -serial null -serial tcp:127.0.0.1:$PORT,server,nowait \
	-monitor ${MONITOR:-none} ${MBX_TRACE:+-global mbx.trace=$MBX_TRACE} "$@"
