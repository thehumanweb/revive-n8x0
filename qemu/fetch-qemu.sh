#!/bin/sh
# SPDX-License-Identifier: MIT
# fetch-qemu.sh [VERSION]: QEMU's release tarball into src/, checked against
# its sha256: 9.1.3 (patches/).
# Not in git; prepare-tree.sh and docker/ use it.
set -eu
cd "$(dirname "$0")"
V=${1:-9.1.3}
case $V in
9.1.3)	F=qemu-9.1.3.tar.xz
	SHA=480a77a0ed13a9b39415f639aa020b4eb0d7cc5a52569510dfd830b3af1bac89 ;;
*)	echo "fetch-qemu.sh: no patches for QEMU $V (9.1.3)" >&2; exit 1 ;;
esac
mkdir -p src
if [ ! -f src/$F ]; then
	curl -fL -o src/$F.part https://download.qemu.org/$F
	mv src/$F.part src/$F
fi
echo "$SHA  src/$F" | sha256sum -c -
