#!/bin/sh
# Piglit with the GLES2 tests only, built in docker image n810-piglit:1 (docker build -t n810-piglit:1 -f
# mesa/docker/Dockerfile.piglit mesa/docker). Source: build/piglit-src (git, shallow), install:
# build/piglit-install. Run it as a background job: it takes several minutes.
set -eu
cd "$(dirname "$0")/../.."
docker image inspect n810-piglit:1 >/dev/null 2>&1 || docker build -t n810-piglit:1 -f mesa/docker/Dockerfile.piglit mesa/docker
[ -d build/piglit-src ] || git clone --depth 1 https://gitlab.freedesktop.org/mesa/piglit.git build/piglit-src
git -C build/piglit-src rev-parse HEAD
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work n810-piglit:1 sh -c "
	cmake -S build/piglit-src -B build/piglit-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=/work/build/piglit-install \
		-DPIGLIT_BUILD_GL_TESTS=OFF -DPIGLIT_BUILD_GLES1_TESTS=OFF -DPIGLIT_BUILD_GLES2_TESTS=ON \
		-DPIGLIT_BUILD_GLES3_TESTS=OFF -DPIGLIT_BUILD_CL_TESTS=OFF -DPIGLIT_BUILD_VK_TESTS=OFF \
		-DPIGLIT_BUILD_DMA_BUF_TESTS=OFF -DPIGLIT_USE_WAFFLE=ON -DPIGLIT_BUILD_GLX_TESTS=OFF &&
	ninja -C build/piglit-build install"
