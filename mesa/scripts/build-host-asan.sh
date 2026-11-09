#!/bin/sh
# The host build of Mesa (build-host.sh) with AddressSanitizer, to find memory errors in the mbx driver and the sim shim: build/mesa-host-asan
# (build tree) and build/mesa-host-asan-install. Run dEQP against it with DEQP_ASAN=1 mesa/scripts/run-deqp.sh ... (the deqp binary itself is
# not instrumented; the runtime is preloaded). The asserts of Mesa and the driver stay on (they found a bug the release build hid);
# ASAN_NDEBUG=true turns them off, as in the release build. Run mesa/scripts/prepare-mesa.sh first if the driver's sources changed.
set -eu
cd "$(dirname "$0")/../.."
M=build/mesa/mesa-26.1.8
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work n810-mesa:1 sh -c "
	[ -d build/mesa-host-asan ] && RECONF=--reconfigure || RECONF=
	meson setup build/mesa-host-asan $M \$RECONF --prefix=/work/build/mesa-host-asan-install --buildtype=debugoptimized \
		-Db_sanitize=address -Db_lundef=false -Db_ndebug=${ASAN_NDEBUG:-false} \
		-Dgallium-drivers=mbx,softpipe -Dvulkan-drivers= -Dllvm=disabled -Dglx=disabled -Degl=enabled \
		-Dgbm=enabled -Dplatforms= -Dgles1=disabled -Dgles2=enabled -Dopengl=false \
		-Dvideo-codecs= -Dgallium-rusticl=false -Dglvnd=disabled \
		-Dlmsensors=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false \
		-Dshader-cache=disabled -Dtools=drm-shim &&
	ninja -C build/mesa-host-asan install"
