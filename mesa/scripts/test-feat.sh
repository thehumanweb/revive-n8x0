#!/bin/sh
# The feature tests (mesa/tests/gles2_feat.c) on the host sim. Needs the host Mesa (build-host.sh).
# Usage: mesa/scripts/test-feat.sh [test name ...]
set -eu
cd "$(dirname "$0")/../.."
L=/work/build/mesa-host-install/lib/x86_64-linux-gnu
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work n810-mesa:1 \
	gcc -O2 -std=gnu11 -Wall -I/work/build/mesa-host-install/include -o build/gles2_feat \
	mesa/tests/gles2_feat.c -L$L -Wl,-rpath-link,$L -lEGL -lGLESv2 -lm
exec mesa/scripts/run-sim.sh ./build/gles2_feat "$@"
