#!/bin/sh
# dEQP-GLES2 on the host sim (needs mesa/scripts/build-deqp.sh and build-host.sh first).
# Usage: mesa/scripts/run-deqp.sh NAME 'dEQP-GLES2.functional.fbo.render.*' [more deqp-gles2 arguments]
# Results: build/deqp-out/NAME.qpa (the log), NAME.txt (the console), summary printed.
set -eu
cd "$(dirname "$0")/../.."
name=${1:?result name}; filter=${2:?case filter (a pattern)}; shift 2
L=/work/build/mesa-host-install/lib/x86_64-linux-gnu
PRE=""
if [ -n "${DEQP_ASAN:-}" ]; then
	L=/work/build/mesa-host-asan-install/lib/x86_64-linux-gnu
	PRE=/usr/lib/x86_64-linux-gnu/libasan.so.8:
fi
mkdir -p build/deqp-out
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work/build/deqp-build/modules/gles2 \
	-e LD_LIBRARY_PATH=$L -e LD_PRELOAD=${PRE}$L/libmbx_sim_drm_shim.so -e ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:halt_on_error=1${ASAN_EXTRA:-} -e MESA_DEBUG -e MBX_DEBUG -e MBX_SIM_REBUILD -e MBX_SIM_TRACE -e MBX_SIM_CRC -e MALLOC_PERTURB_ -e MALLOC_CHECK_ -e MBX_DEBUG_FMT -e DEQP_IMAGES -e DEQP_SHADERS -e DEQP_SURFACE -e DEQP_WATCHDOG -e MBX_DUMP_NIR -e MBX_DUMP_NIR_IN -e MBX_SIM_NOCLAMP -e MBX_NO_GRID_SUMS -e SOFTPIPE_DEBUG \
	n810-piglit:1 sh -c "./deqp-gles2 --deqp-surface-type=${DEQP_SURFACE:-fbo} --deqp-watchdog=${DEQP_WATCHDOG:-disable} --deqp-gl-config-name=rgba8888d24s8 --deqp-surface-width=512 --deqp-surface-height=512 --deqp-log-images=${DEQP_IMAGES:-disable} --deqp-log-shader-sources=${DEQP_SHADERS:-disable} \
		--deqp-log-filename=/work/build/deqp-out/$name.qpa --deqp-case='$filter' $* > /work/build/deqp-out/$name.txt 2>&1; \
		grep -E 'Passed|Failed|Not supported|Warnings|Test case.*:' /work/build/deqp-out/$name.txt | tail -8"
