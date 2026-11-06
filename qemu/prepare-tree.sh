#!/bin/sh
# SPDX-License-Identifier: MIT
# prepare-tree.sh DIR [--mbx]: a QEMU source tree in DIR/qemu-VERSION with
# the patches applied (QEMU_VERSION=9.1.3, the default: patches/), and with --mbx the PowerVR MBX
# device and its emulator core put in (device/install.py). Configure
# and build it as README.md says.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dir=$1
V=${QEMU_VERSION:-9.1.3}
case $V in
9.1.3)	P=$here/patches T=qemu-9.1.3.tar.xz ;;
*)	echo "prepare-tree.sh: no patches for QEMU $V" >&2; exit 1 ;;
esac
"$here/fetch-qemu.sh" $V
mkdir -p "$dir"
cd "$dir"
tar xf "$here/src/$T"
cd qemu-$V
for p in "$P"/*.patch; do
	patch -p1 -s < "$p"
done
if [ "${2:-}" = --mbx ]; then
	python3 "$here/device/install.py" .
fi
echo "$dir/qemu-$V: patched${2:+, with the MBX}"
