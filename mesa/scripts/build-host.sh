#!/bin/sh
# Host (x86) build of Mesa with only the mbx gallium driver, in docker image
# n810-mesa:1 (docker build -t n810-mesa:1 mesa/docker). Output: build/mesa-host
# (build tree) and build/mesa-host-install (prefix). GLES2 and EGL (surfaceless,
# GBM) only: the sim tests run through the drm-shim in mesa/shim.
set -eu
cd "$(dirname "$0")/../.."
M=build/mesa/mesa-26.1.8
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work n810-mesa:1 sh -c "
	[ -d build/mesa-host ] && RECONF=--reconfigure || RECONF=
	meson setup build/mesa-host $M \$RECONF --prefix=/work/build/mesa-host-install --buildtype=release \
		-Dgallium-drivers=mbx -Dvulkan-drivers= -Dllvm=disabled -Dglx=disabled -Degl=enabled \
		-Dgbm=enabled -Dplatforms= -Dgles1=disabled -Dgles2=enabled -Dopengl=false \
		-Dvideo-codecs= -Dgallium-rusticl=false -Dglvnd=disabled \
		-Dlmsensors=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false \
		-Dshader-cache=disabled -Dtools= &&
	ninja -C build/mesa-host install"
