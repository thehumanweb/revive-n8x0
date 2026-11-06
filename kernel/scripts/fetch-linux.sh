#!/bin/sh
# SPDX-License-Identifier: MIT
# fetch-linux.sh: the Linux 6.18.58 tarball into src/, checked against the
# sha256 kernel.org publishes for it. Not in git; prepare-6.18.sh uses it.
set -eu
cd "$(dirname "$0")/.."
V=6.18.58
F=linux-$V.tar.xz
SHA=${LINUX_SHA256:-}
mkdir -p src
if [ ! -f src/$F ]; then
	curl -fL -o src/$F.part https://cdn.kernel.org/pub/linux/kernel/v6.x/$F
	mv src/$F.part src/$F
fi
if [ -z "$SHA" ]; then
	SHA=$(curl -fsSL https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc | awk -v f=$F '$2==f {print $1}')
fi
[ -n "$SHA" ] || { echo "fetch-linux.sh: no sha256 for $F" >&2; exit 1; }
echo "$SHA  src/$F" | sha256sum -c -
