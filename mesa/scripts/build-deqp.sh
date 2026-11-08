#!/bin/sh
# dEQP-GLES2 (Khronos' VK-GL-CTS) built in docker image n810-piglit:1 against the host Mesa
# (build/mesa-host-install): source build/deqp-src (git, shallow, pinned tag, with its external sources),
# binary build/deqp-build/modules/gles2/deqp-gles2. Run it as a background job: it takes tens of minutes.
#
# It runs two things from the network: git clone of the CTS (github.com/KhronosGroup/VK-GL-CTS, tag below) and
# external/fetch_sources.py of that checkout, which downloads the CTS's external sources (glslang, SPIRV tools and headers,
# libpng, zlib, jsoncpp, ...) at the revisions the CTS pins. Everything lands under build/ (ignored by git); the build runs
# in the container as the calling user.
set -eu
cd "$(dirname "$0")/../.."
TAG=vulkan-cts-1.3.10.0
docker image inspect n810-piglit:1 >/dev/null 2>&1 || docker build -t n810-piglit:1 -f mesa/docker/Dockerfile.piglit mesa/docker
[ -d build/deqp-src ] || git clone --depth 1 --branch $TAG https://github.com/KhronosGroup/VK-GL-CTS.git build/deqp-src
git -C build/deqp-src rev-parse HEAD
L=/work/build/mesa-host-install
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work/build/deqp-src n810-piglit:1 sh -c "
	python3 external/fetch_sources.py &&
	cmake -S . -B /work/build/deqp-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=surfaceless \
		-DCMAKE_PREFIX_PATH=$L -DCMAKE_INCLUDE_PATH=$L/include -DCMAKE_LIBRARY_PATH=$L/lib/x86_64-linux-gnu \
		-DDEQP_GLES2_LIBRARIES=$L/lib/x86_64-linux-gnu/libGLESv2.so -DDEQP_EGL_LIBRARIES=$L/lib/x86_64-linux-gnu/libEGL.so &&
	ninja -C /work/build/deqp-build deqp-gles2"
