# qemu-n8x0

QEMU for the Nokia N800/N810 (OMAP2420), with a model of the N810's
PowerVR MBX1 graphics core.

- `qemu/patches/`: 21 patches to QEMU 9.1.3's `n800`/`n810` machines,
  9.1 being the last release with them (QEMU removed them in 9.2): UART,
  PRCM, DSS/RFBI, DMA, I2C, watchdog, McSPI, MIPI display, TUSB6010/MUSB
  host and gadget, LM8323 keyboard, RNG, sensors, MMC, switches, the real
  partition table. Enough for Linux 6.18 to boot to a shell
  with the RFBI panel, keyboard, touchscreen, eMMC and USB
  networking.
- `kernel/`: Linux 6.18.58 for the N810: 62 patches, configs, build scripts
  and a kexec loader; see `kernel/README.md`.
- `mbx/`: mbxemu, a functional model of the PowerVR MBX1 (TA, VGP, 3D
  core, 2D core, parameter buffer), see `mbx/README.md`; its QEMU device is in
  `qemu/device/`. Checked against the N810 frame by frame and
  word by word.
- `qemu/docker/Dockerfile.9.1`: the `n810-qemu:9.1` image (QEMU 9.1.3 with the
  patches, without the MBX; it keeps the build tools for MBX trees).

## Build

```sh
qemu/fetch-qemu.sh                               # qemu/src/qemu-9.1.3.tar.xz, sha256-checked
docker build --build-arg UID=$(id -u) --build-arg GID=$(id -g) \
    -f qemu/docker/Dockerfile.9.1 -t n810-qemu:9.1 qemu   # your UID, to use files you mount
```

`-drive if=mtd,format=raw,file=...` gives the OneNAND image (`-mtdblock`
works too, with a raw-probing warning); USB networking is
`-net nic,model=tusb6010` (or `-nic user,model=tusb6010,...`).

With the MBX (a tree to configure and build with that image, as
`mbx/README.md` shows):

```sh
qemu/prepare-tree.sh ~/qemu-mbx --mbx                          # 9.1.3, meson
```

The emulator core alone (x86 host, no QEMU needed):

```sh
cd mbx && make
```

## Licences

The emulator (`mbx/`, except `mbx/kmod/`) and the scripts are MIT (SPDX
headers). `mbx/kmod/` is Imagination Technologies' MBX kernel driver
source, GPL-2.0 (see `mbx/kmod/COPYING`; each file keeps its own notice).
The kernel patches in `kernel/patches/` are GPL-2.0 as changes to Linux. The patches
in `qemu/patches/` modify QEMU and are under QEMU's licence, GPL-2.0, as
is QEMU built with the MBX device in it.
