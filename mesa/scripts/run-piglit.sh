#!/bin/sh
# Piglit's GLES2 tests (mesa/tests/piglit-gles2.txt: the binaries, the glsl-es-1.00 compiler tests and the
# es 2.0 ones) on the host sim: waffle's surfaceless EGL, Mesa from build/mesa-host-install with the mbx
# drm-shim preloaded. Needs mesa/scripts/build-piglit.sh and build-host.sh first.
# Usage: [TESTLIST=file] [JOBS=n] mesa/scripts/run-piglit.sh RESULTS_DIR [extra piglit run arguments]
set -eu
cd "$(dirname "$0")/../.."
out=${1:?results dir}; shift
L=/work/build/mesa-host-install/lib/x86_64-linux-gnu
rm -rf "$out"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work \
	-e LD_LIBRARY_PATH=$L -e LD_PRELOAD=$L/libmbx_sim_drm_shim.so -e MESA_DEBUG -e MBX_DEBUG \
	n810-piglit:1 sh -c "python3 build/piglit-install/bin/piglit run -p surfaceless_egl -j ${JOBS:-4} --timeout 120 \
	--test-list ${TESTLIST:-mesa/tests/piglit-gles2.txt} $* all /work/$out && \
	python3 build/piglit-install/bin/piglit summary console /work/$out | tail -15"
