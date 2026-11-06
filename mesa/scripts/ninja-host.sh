#!/bin/sh
# Incremental rebuild and install of the host Mesa (after prepare-mesa.sh --sync)
set -eu
cd "$(dirname "$0")/../.."
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -w /work n810-mesa:1 \
	sh -c "ninja -C build/mesa-host install" 2>&1
