#!/bin/sh
# dEQP-GLES2 for the N810 (ARM11, the Buildroot toolchain), from the CTS checkout build/deqp-src that build-deqp.sh fetched
# (no network here): cmake and ninja of docker image n810-piglit:1 with the cross compiler, libEGL/libGLESv2 and headers
# of Buildroot's sysroot (rootfs/build/br-out-armel, so build the device image first). Output: build/deqp-arm/modules/gles2/
# (deqp-gles2 and its gles2/ data directory, to be copied to the unit); the binary is stripped to build/deqp-arm/deqp-gles2.stripped.
# Run it as a background job.
set -eu
cd "$(dirname "$0")/../.."
B=/work/rootfs/build/br-out-armel
S=$B/host/arm-buildroot-linux-gnueabi/sysroot
[ -d build/deqp-src/external/libpng ] || { echo "run mesa/scripts/build-deqp.sh first (it fetches the CTS)" >&2; exit 1; }
[ -d rootfs/build/br-out-armel/host ] || { echo "build the device image first (rootfs/scripts/build-rootfs.sh)" >&2; exit 1; }
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work/build/deqp-src n810-piglit:1 sh -c "
	cmake -S . -B /work/build/deqp-arm -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_TOOLCHAIN_FILE=$B/host/share/buildroot/toolchainfile.cmake -DDEQP_TARGET=surfaceless \
		-DCMAKE_PREFIX_PATH=$S/usr -DCMAKE_INCLUDE_PATH=$S/usr/include -DCMAKE_LIBRARY_PATH=$S/usr/lib \
		-DDEQP_GLES2_LIBRARIES=$S/usr/lib/libGLESv2.so -DDEQP_EGL_LIBRARIES=$S/usr/lib/libEGL.so \
		-DDE_CPU=DE_CPU_ARM -DDE_PTR_SIZE=4 &&
	ninja -C /work/build/deqp-arm deqp-gles2 &&
	$B/host/bin/arm-buildroot-linux-gnueabi-strip -o /work/build/deqp-arm/deqp-gles2.stripped /work/build/deqp-arm/modules/gles2/deqp-gles2"
ls -l build/deqp-arm/deqp-gles2.stripped
