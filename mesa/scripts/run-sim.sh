#!/bin/sh
# Run a command on the host sim: Mesa from build/mesa-host-install with the mbx
# drm-shim preloaded, in the n810-mesa image. Paths in the command are relative to
# the repo root. Example: mesa/scripts/run-sim.sh build/gles2_tri -o build/sim.ppm
# MBX_DEBUG-style variables (EGL_LOG_LEVEL, MESA_DEBUG, DRM_SHIM_DEBUG) pass through.
set -eu
cd "$(dirname "$0")/../.."
L=/work/build/mesa-host-install/lib/x86_64-linux-gnu
exec docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work \
	-e LD_LIBRARY_PATH=$L -e LD_PRELOAD=$L/libmbx_sim_drm_shim.so \
	-e EGL_LOG_LEVEL -e MBX_DEBUG -e MBX_SIM_TRACE -e MESA_DEBUG -e DRM_SHIM_DEBUG -e MESA_LOADER_DRIVER_OVERRIDE \
	${SIM_IMAGE:-n810-mesa:1} "$@"
