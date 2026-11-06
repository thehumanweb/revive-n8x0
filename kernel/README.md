# kernel: Linux 6.18 for the Nokia N810

Linux 6.18.58 with 62 patches for the N800/N810 (`patches/6.18/`), kernel
configs, build scripts, a small kexec loader, and the MBX access driver
(`../mbx/kmod`) built against it. The kernel boots on the patched QEMU in
`../qemu` and is meant for the real device.

| Path | |
|---|---|
| `patches/6.18/` | the patches on top of 6.18.58: device tree, display (RFBI/S1D1374x), power management, input, p54spi wifi, audio, Retu/Tahvo, MTD/OneNAND, ARMv6 fixes |
| `config/` | config fragments merged over `omap2plus_defconfig`: `local-6.18`, `size-6.18` (fits NOLO's 2 MB kernel partition), `loader-6.18`, and `debug-6.18`, `test-*` for experiments |
| `scripts/` | `fetch-linux.sh`, `prepare-6.18.sh`, `build-6.18.sh`, `build-mbx.sh`, `build-loader.sh` |
| `loader/init.c` | the loader: a static, no-libc `/init` in a tiny kernel that NOLO boots; it mounts the rootfs and kexecs `/boot/zImage` (or `zImage-n800`) |
| `docker/` | `kbuild-6` (cross toolchain, image `n810-kbuild:6`) and `buildroot` (static compiler for the loader) |

## Build

Run from `kernel/`:

```sh
docker build --build-arg UID=$(id -u) --build-arg GID=$(id -g) \
    -t n810-kbuild:6 docker/kbuild-6
scripts/fetch-linux.sh      # src/linux-6.18.58.tar.xz, sha256-checked
scripts/prepare-6.18.sh     # extract, apply patches, configure (optional fragment: config/debug-6.18)
scripts/build-6.18.sh       # out/zImage-6.18 (N810 DTB appended), out/zImage-6.18-n800
scripts/build-mbx.sh        # out/mbxaccess.ko
scripts/build-loader.sh     # out/zImage-loader (needs the n810-buildroot image and the rootfs build's toolchain, `../rootfs`)
```

`out/zImage-6.18` is what QEMU's `-kernel` takes (the DTB is appended because
NOLO and QEMU pass ATAGs, not a DTB). Builds are incremental, with objects in
`build/`. `build-6.18.sh` warns when the image no longer fits the kernel
partition.

`scripts/build-loader.sh` compiles the loader with the toolchain of the
Buildroot build in `../rootfs` (`BR_OUT` overrides the path).

## Licence

The patches are GPL-2.0, as changes to the Linux kernel; `loader/init.c` is
GPL-2.0 (SPDX header). The scripts and Dockerfiles are MIT.
