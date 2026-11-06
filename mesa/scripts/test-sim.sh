#!/bin/sh
# The GLES2 triangle on the host sim against the emulator-only picture: builds
# mesa/tests/gles2_tri.c and mbx/mbxtri, runs both at several angles and compares
# the PPMs byte for byte. Needs the host Mesa built (build-host.sh / ninja-host.sh).
set -eu
cd "$(dirname "$0")/../.."
L=/work/build/mesa-host-install/lib/x86_64-linux-gnu
make -C mbx mbxtri >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work n810-mesa:1 \
	gcc -O2 -std=gnu11 -Wall -I/work/build/mesa-host-install/include -o build/gles2_tri \
	mesa/tests/gles2_tri.c -L$L -Wl,-rpath-link,$L -lEGL -lGLESv2 -lm
fail=0
for a in ${ANGLES:-0 30 90 200}; do
	mbx/mbxtri -a $a -o build/host-$a.ppm 2>/dev/null
	mesa/scripts/run-sim.sh ./build/gles2_tri -a $a -o build/sim-$a.ppm 2>/dev/null
	if cmp -s build/sim-$a.ppm build/host-$a.ppm; then
		echo "angle $a: identical"
	else
		echo "angle $a: DIFFERENT ($(cmp -l build/sim-$a.ppm build/host-$a.ppm | wc -l) bytes)"
		fail=1
	fi
done
exit $fail
