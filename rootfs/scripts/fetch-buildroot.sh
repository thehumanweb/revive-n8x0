#!/bin/sh
# SPDX-License-Identifier: MIT
# fetch-buildroot.sh: the Buildroot tarball into src/, checked against the
# sha256 Buildroot publishes in the release's .sign file. Not in git.
set -eu
cd "$(dirname "$0")/.."
V=2026.08.1
F=buildroot-$V.tar.xz
SHA=34fb7bea8a213ffde39cc02e61703391560f26e0850f03138c722d0b09880f2b
mkdir -p src
if [ ! -f src/$F ]; then
	curl -fL -o src/$F.part https://buildroot.org/downloads/$F
	mv src/$F.part src/$F
fi
echo "$SHA  src/$F" | sha256sum -c -
