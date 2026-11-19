# N8x0 Revival

> **EXPERIMENTAL. This is research software and it may damage your device.** It replaces the system software of the
> unit (kernel, kernel partition loader, root filesystem), including the code that talks to the power and charging
> hardware, and drives the display and graphics hardware directly. It can leave a unit unbootable, lose data, or harm
> the battery. Use it at your own risk and back your unit up first.
>
> **Tested only on a Nokia N810 with an AC-8E charger.** Nothing here has been run on an N800 (the N800 kernel and
> loader are built, not tested) or with other chargers or batteries.

The Nokia N800 and N810 (OMAP2420) on a current software stack: Linux 6.18, OpenGL ES 2.0 on the N810's PowerVR MBX1
through a Mesa Gallium driver and a kernel DRM driver, Weston on the panel, and QEMU to develop all of it without the
device. The build and install instructions are below; `JOURNEY.md` is the log of how each piece was found (with
sources), `TODO.md` what is open, and `mesa/COVERAGE.md` what the GLES2 driver does on the chip and what falls back to
the CPU.

- `kernel/`: Linux 6.18.58 for the N800 and N810: 65 patches, configs, build scripts, a kexec loader and the build of
  the MBX DRM driver (`mbx/drm`); see `kernel/README.md`. It builds **two kernels from one source**: `zImage-6.18`
  (the N810's device tree appended) and `zImage-6.18-n800` (the N800's). The N800 kernel is built and staged on the
  device image, which the loader picks by NOLO's product tag, but this repository has no record of it being run on an
  N800.
- `mbx/`: mbxemu, a functional model of the PowerVR MBX1 (TA, VGP, 3D core, 2D core, parameter buffer), see
  `mbx/README.md`; its QEMU device is in `qemu/device/`. Checked against the N810 frame by frame and word by word.
  The kernel DRM driver for the chip is `mbx/drm`.
- `mesa/`: the `mbx` Gallium driver for Mesa 26.1.8 (GLES2, EGL, GBM), its tests and the piglit and dEQP scripts.
- `rootfs/`: the Buildroot root filesystem (Mesa with the driver, Weston, the test programs) and the device image.
- `qemu/patches/`: 21 patches to QEMU 9.1.3's `n800`/`n810` machines, 9.1 being the last release with them (QEMU
  removed them in 9.2): UART, PRCM, DSS/RFBI, DMA, I2C, watchdog, McSPI, MIPI display, TUSB6010/MUSB host and gadget,
  LM8323 keyboard, RNG, sensors, MMC, switches, the real partition table. Enough for Linux 6.18 to boot to a shell
  with the RFBI panel, keyboard, touchscreen, eMMC and USB networking.
- `qemu/docker/Dockerfile.9.1`: the `n810-qemu:9.1` image (QEMU 9.1.3 with the patches, without the MBX; it keeps the
  build tools for MBX trees).

## Requirements

Everything is built inside Ubuntu 24.04 containers (Dockerfiles in this repository), so the host only needs
Docker and a few tools. Written for and tested on **Ubuntu 24.04 LTS, amd64**.

- A 64-bit x86 PC with 8 cores and 16 GB of RAM (4 cores and 8 GB work, slower), plenty of free disk for the build
  trees and downloads (a Linux tree and a Buildroot build are large), and internet access for the first build.
- Host packages:

  ```sh
  sudo apt update
  sudo apt install docker.io git curl python3 build-essential
  sudo usermod -aG docker $USER        # then log out and in again, so `docker` works without sudo
  git clone https://github.com/thehumanweb/revive-n8x0 && cd revive-n8x0
  ```

  `docker.io` runs every toolchain (images `n810-kbuild:6`, `n810-buildroot`, `n810-mesa:1`, `n810-piglit:1` and
  `n810-qemu:9.1`, built from `kernel/docker/`, `mesa/docker/` and `qemu/docker/`), `curl` and `python3` fetch and
  check the sources, and `build-essential` is for `make` in `mbx/` and for building QEMU on the host.
- To run the machine in QEMU on the host, also the packages QEMU needs to build:

  ```sh
  sudo apt install pkg-config python3-venv ninja-build zlib1g-dev libglib2.0-dev libpixman-1-dev libfdt-dev \
      libslirp-dev flex bison
  ```

  Ubuntu's own `qemu-system-arm` (8.2) does not do: the N810 patches and the MBX device are for QEMU 9.1.3.
- To install on a real N810 (RX-44): a USB cable, the packages `sshpass 0xffff` on the machine the unit is plugged
  into (the "jig host", which may be the build machine), and a backup first (see "Install on a Nokia N810").

The first build downloads (each checked against a pinned sha256 or the publisher's signed list): QEMU 9.1.3, Linux
6.18.58, Buildroot 2026.08.1 and the packages it builds, Mesa 26.1.8. The optional test suites fetch more (piglit, and
dEQP from Khronos' VK-GL-CTS, which downloads its own external sources).

## Build

Run from the repository root, in this order (each step's outputs are named; a step that fails stops with a message,
but check its exit code: a failed rootfs build leaves the previous image in `rootfs/out/`).

**1. QEMU** with the N810 machine and the MBX device:

```sh
qemu/prepare-tree.sh build/qemu-src --mbx          # fetches QEMU 9.1.3 (sha256-checked), patches it, adds the MBX
mkdir build/qemu-src/qemu-9.1.3/build && cd build/qemu-src/qemu-9.1.3/build
../configure --target-list=arm-softmmu --disable-werror --disable-docs \
    --disable-sdl --disable-gtk --disable-vnc --enable-slirp --disable-download
ninja qemu-system-arm                              # build/qemu-src/qemu-9.1.3/build/qemu-system-arm
cd ../../../..
```

(Without the MBX, QEMU 9.1.3 with the N810 patches is also a container image:
`qemu/fetch-qemu.sh; docker build --build-arg UID=$(id -u) --build-arg GID=$(id -g) -f qemu/docker/Dockerfile.9.1 -t n810-qemu:9.1 qemu`.)

**2. Kernel** Linux 6.18.58 with the 65 patches, and the MBX DRM driver (about 15 minutes from clean):

```sh
docker build --build-arg UID=$(id -u) --build-arg GID=$(id -g) -t n810-kbuild:6 kernel/docker/kbuild-6
cd kernel
scripts/fetch-linux.sh      # src/linux-6.18.58.tar.xz, checked against kernel.org's signed list
scripts/prepare-6.18.sh     # extract, apply the patches, configure
scripts/build-6.18.sh       # out/zImage-6.18 (N810 device tree appended), out/zImage-6.18-n800, modules in build/modules-6.18
scripts/build-mbx.sh        # out/mbxaccess.ko (Imagination's driver source, for the stock-driver tests)
cd ..
```

`kernel/README.md` has the details (configs, the 2 MB kernel partition limit that `build-6.18.sh` warns about).

**3. The emulator core and its tests** (no QEMU needed):

```sh
make -C mbx
```

**4. Mesa with the mbx driver, on the host** (the simulator the GLES2 tests run on):

```sh
docker build -t n810-mesa:1 mesa/docker
mesa/scripts/prepare-mesa.sh        # Mesa 26.1.8 (sha256-checked) with this repository's patches and driver
mesa/scripts/build-host.sh          # build/mesa-host-install
mesa/scripts/test-feat.sh           # the feature tests on the simulator: every line should say PASS
```

**5. The root filesystem** (Buildroot: cross GCC, Mesa for ARM with the mbx driver, Weston, the test programs; more
than an hour from clean, downloads cached in `rootfs/build/dl`). Needs step 2 first (the kernel's modules go into it):

```sh
docker build -t n810-buildroot kernel/docker/buildroot
rootfs/scripts/build-rootfs.sh      # rootfs/out/rootfs.{tar,ext4,jffs2}; the log is rootfs/build/build-rootfs.log
```

**6. The kexec loader** (only for installing on a device; needs step 5's toolchain):

```sh
kernel/scripts/build-loader.sh      # kernel/out/zImage-loader (N810) and zImage-loader-n800 (N800: built, not run on an N800)
```

## Run in QEMU

```sh
scripts/run-qemu.sh                 # builds build/flash.img from rootfs/out/rootfs.jffs2, boots the N810
nc 127.0.0.1 5577                   # the console (third UART): log in as root, no password
```

`PORT=`, `QEMU=`, `MONITOR=unix:build/mon.sock,server,nowait` (a QEMU monitor, for screendumps) and `MBX_TRACE=1|2`
(log the MBX register accesses) are the options; the header of `scripts/run-qemu.sh` lists them. The guest has
the MBX device, `mbx.ko` loads at boot and the demo programs (`gles2_tri`, `gles2_feat`, `gbm_tri`) are in `/usr/bin`.

## Tests

- **Feature tests on the host simulator:** `mesa/scripts/test-feat.sh [name ...]` (about 64 tests, a minute or two),
  and `mesa/scripts/run-sim.sh COMMAND` runs any program against the simulated chip.
- **Piglit's GLES2 set:** `mesa/scripts/build-piglit.sh` (clones piglit), then `mesa/scripts/run-piglit.sh build/piglit-out`;
  the expected result is in `mesa/tests/piglit-baseline.txt`.
- **dEQP-GLES2:** `mesa/scripts/build-deqp.sh` clones Khronos' VK-GL-CTS and runs its own source-fetch script (read the
  comment at the top of the script first), then `mesa/scripts/run-deqp.sh NAME 'dEQP-GLES2.functional.fbo.*'` or
  `run-deqp-batch.sh JOBS group ...` and `deqp-summary.sh`. It runs with a 512 x 512 surface because the chip's address
  space is 32 MB a process (`mbx/README.md`).
- The same feature tests run on the device itself: `gles2_feat` in the image, on the unit's own chip.

## Install on a Nokia N810

`docs/BUILD-AND-FLASH.md` is a shorter guide from a clean checkout to the flashable image and the flash, with a check after each step.

This is experimental (see the warning at the top: tested only on an N810 with an AC-8E charger); flashing can leave a unit
unbootable. **Back up the unit first** (at least the CAL and the
rootfs partition; `0xFFFF` can read partitions, see its manual). The scripts here only write the **rootfs partition**:
the bootloader (NOLO), the initfs and the CAL stay as they are.

**What the unit needs.** The N810's stock flow is NOLO, the kernel partition, the initfs, then the rootfs. Here the
kernel partition holds the small kexec loader (`kernel/out/zImage-loader`, built in step 6; an N800 takes
`zImage-loader-n800`, which has the N800's device tree: untested on an N800), which mounts the rootfs
and `kexec`s `/boot/zImage` from it (`/boot/zImage-n800` on an N800), with `/boot/zImage.old` as its fallback; the stock
initfs stays and hands over to our `init`. Putting the loader into the kernel partition is **not scripted here** (with `0xFFFF`
it is a `-m kernel:...` flash: read `0xFFFF -h`, and know that the unit then depends on the loader and on whatever
`zImage.old` is until you have a rootfs you trust). The scripts below assume a unit that already has the loader.

**The jig host** is any Linux machine with the unit on USB. It needs `0xFFFF` and `sshpass`, and a small helper,
`~/n810/n810-net.sh`, that finds the unit's USB networking interface and runs a command on it. The unit's
kernel presents a USB ethernet gadget (vendor id `0525`): the host side is `192.168.2.14/24`, the unit
`192.168.2.15`, `ssh root@192.168.2.15` (password `n810`). A working helper:

```sh
#!/bin/sh
# usage: sh n810-net.sh [command ...]
IF=$(for i in /sys/class/net/*; do
	[ "$(cat $i/device/../idVendor 2>/dev/null)" = 0525 ] && basename $i
done | head -1)
[ -n "$IF" ] || { echo "no N810 USB network interface" >&2; exit 1; }
ip -4 addr show dev $IF | grep -q 192.168.2.14 ||
	{ sudo ip addr add 192.168.2.14/24 dev $IF && sudo ip link set $IF up; }
[ $# -eq 0 ] && { echo "$IF up"; exit 0; }
exec sshpass -p n810 ssh -o ConnectTimeout=30 -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@192.168.2.15 "$@"
```

**Steps**, from the repository root (the scripts take the jig host as `JIG_HOST=user@host`; if the build machine is
the jig host, `JIG_HOST=$USER@localhost` works):

```sh
# 1. a fallback kernel for the loader: the kernel the unit runs now, copied from it (needs the unit up and n810-net.sh)
JIG_HOST=user@jig-host rootfs/scripts/fetch-device-kernel.sh      # rootfs/build/device-zImage.old
#    no unit to copy from yet: cp kernel/out/zImage-6.18 rootfs/build/device-zImage.old   (a stopgap that is no fallback at all)

# 2. the device image: the device overlay, root password n810, /boot from the kernels of step 2 (about 30 MB)
DEVICE=1 rootfs/scripts/build-rootfs.sh                            # rootfs/out/rootfs-device.jffs2

# 3. copy it and the helper scripts to ~/n810/mbx-test on the jig host
JIG_HOST=user@jig-host scripts/device/stage-on-jig.sh

# 4. flash: start the flasher, then reboot the unit (power menu, or `reboot` over ssh): NOLO takes the image over USB
ssh user@jig-host 'cd ~/n810/mbx-test && sh flash-mbx.sh'          # 0xFFFF -m rootfs:rootfs-device.jffs2 -f -r
```

Unplug the charger first unless you mean to charge under this system. After the flash the unit reboots by itself.

**First boot.** Log in as `root` (password `n810`) with `ssh root@192.168.2.15` over USB networking (no serial cable
is needed). The kernel and the login prompt also run on the unit's serial console (the third UART, 115200 8N1), for
anyone who has a serial jig (see https://bues.ch/cms/unmaintained/n810-serial.html). Weston starts at boot on the panel (`/etc/init.d/S90weston`;
`touch /etc/noweston` turns that off) and `weston-simple-egl`, `weston-terminal` and the other Weston clients are
installed. The GPU driver is loaded at boot; if `dmesg | grep mbx` is empty, `modprobe mbx` loads it, and
`touch /etc/mbx-autoload` makes `S35mbx` load it at every boot. The feature tests: `gles2_feat` (64 tests).

**Getting back.** If `/boot/zImage` does not boot, the loader falls back to `/boot/zImage.old`. If the rootfs itself is wrong, flash a good image with the same steps (the flasher window is
there whenever you reboot into NOLO), or restore the backup you took.

## Licences

The emulator (`mbx/`, except `mbx/kmod/`) and the scripts are MIT (SPDX
headers). `mbx/kmod/` is Imagination Technologies' MBX kernel driver
source, GPL-2.0 (see `mbx/kmod/COPYING`; each file keeps its own notice).
The kernel patches in `kernel/patches/` are GPL-2.0 as changes to Linux. The patches
in `qemu/patches/` modify QEMU and are under QEMU's licence, GPL-2.0, as
is QEMU built with the MBX device in it.
