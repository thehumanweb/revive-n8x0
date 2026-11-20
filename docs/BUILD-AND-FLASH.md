# From a clean checkout to a flashable N810 image

This is the shortest path through the repository to **`rootfs/out/rootfs-device.jffs2`**, the file that goes into
the N810's rootfs partition, and then onto the unit. The README has the same steps with more background; this page
keeps only what the image needs and says what each step produces and how to check it.

> **Experimental.** Flashing replaces the unit's system software, including the code that talks to the power and
> charging hardware. It can leave a unit unbootable or lose data. It has been tested only on a Nokia N810 with an AC-8E
> charger. Back the unit up first and unplug the charger before flashing (see "Before you flash").

## What you end up with

| File | What it is | Made by |
|---|---|---|
| `kernel/out/zImage-6.18` | Linux 6.18.58 for the N810 (device tree appended) | `kernel/scripts/build-6.18.sh` |
| `kernel/out/zImage-6.18-n800` | the same for an N800 (built, never run on one) | same |
| `rootfs/build/device-zImage.old` | the kernel the unit runs *now*, the loader's fallback | `rootfs/scripts/fetch-device-kernel.sh` |
| `rootfs/out/rootfs-device.jffs2` | the device root filesystem: Buildroot, Mesa with the `mbx` driver, Weston, the kernels in `/boot`, padded to 17 MiB | `DEVICE=1 rootfs/scripts/build-rootfs.sh` |
| `kernel/out/zImage-loader` | the small kexec loader that lives in the unit's **kernel partition** | `kernel/scripts/build-loader.sh` |

Only `rootfs-device.jffs2` is flashed by the scripts here. The loader is built, but putting it into the kernel
partition is **not scripted** (see "What this guide does not cover").

## Prerequisites

- A 64-bit x86 machine with Ubuntu 24.04 (what it was written on), 8 cores and 16 GB of RAM (4 cores and 8 GB work,
  slower), plenty of free disk, and internet for the first build.
- `sudo apt install docker.io git curl python3 build-essential`, and your user in the `docker` group.
- For flashing: a USB cable, and on the machine the unit is plugged into (the "jig host", which may be the build
  machine) the packages `sshpass` and `0xffff`.

Everything else (cross compiler, Mesa, Buildroot) is built inside containers from this repository. Sources are
downloaded and checked against pinned sha256 sums or the publisher's signed lists.

## Steps

Run from the repository root. A step that fails stops with a message, but check its exit code: a failed rootfs build
leaves the **previous** image in `rootfs/out/`, so check the file's date after the build.

### 1. Get the repository

```sh
git clone https://github.com/thehumanweb/revive-n8x0 && cd revive-n8x0
```

### 2. Build the kernel (about 15 minutes from clean)

```sh
docker build --build-arg UID=$(id -u) --build-arg GID=$(id -g) -t n810-kbuild:6 kernel/docker/kbuild-6
cd kernel
scripts/fetch-linux.sh      # Linux 6.18.58, checked against kernel.org's signed list
scripts/prepare-6.18.sh     # extract, apply the patches in kernel/patches/6.18, configure
scripts/build-6.18.sh       # kernel/out/zImage-6.18, zImage-6.18-n800, modules in kernel/build/modules-6.18
cd ..
```

Check: both `kernel/out/zImage-6.18*` files exist. `build-6.18.sh` warns if a kernel is over the 2 MB kernel-partition
limit; that matters only if you put a kernel (not the loader) into that partition. `kernel/README.md` has the details.

### 3. Get a fallback kernel

The loader boots `/boot/zImage` from the rootfs and falls back to `/boot/zImage.old` if that fails, so the image
needs a kernel you know boots. The scripts copy the one the unit runs now:

```sh
JIG_HOST=user@jig-host rootfs/scripts/fetch-device-kernel.sh      # -> rootfs/build/device-zImage.old
```

This needs the unit up and the helper `~/n810/n810-net.sh` on the jig host (the README shows a working one). The
scripts need `JIG_HOST` and stop if it is not set.

Without a unit to copy from, `cp kernel/out/zImage-6.18 rootfs/build/device-zImage.old` lets the build go on, but
then the fallback is the same kernel as the main one: no fallback at all.

### 4. Build the device root filesystem (more than an hour from clean)

```sh
docker build -t n810-buildroot kernel/docker/buildroot
DEVICE=1 rootfs/scripts/build-rootfs.sh       # rootfs/out/rootfs-device.jffs2; the log is rootfs/build/build-rootfs.log
```

This also prepares Mesa 26.1.8 with this repository's driver and patches, cross-builds the test programs, and stages
`/boot` from steps 2 and 3. Downloads are cached in `rootfs/build/dl`. The device image has the root password `n810`
and USB networking on `192.168.2.15`.

Check:

```sh
ls -l rootfs/out/rootfs-device.jffs2     # dated now; exactly 17825792 bytes (17 MiB) when the image is smaller than that
sha256sum rootfs/out/rootfs-device.jffs2 # note it: you will compare it on the jig host
```

NOLO and `0xFFFF` treat a JFFS2 image under 16 MiB as an initfs, which is why the script pads the file with erased
flash to 17 MiB.

### 5. Optional: try the same system in QEMU first

```sh
# needs QEMU 9.1.3 with the N810 patches and the MBX device (README, Build step 1), and the plain image:
rootfs/scripts/build-rootfs.sh   # without DEVICE=1: rootfs/out/rootfs.jffs2 (the device build does not make it)
scripts/run-qemu.sh              # boots rootfs/out/rootfs.jffs2; console: nc 127.0.0.1 5577
```

This runs the same kernel, Weston stack and Mesa driver against the simulated chip. It does not exercise the device
overlay (USB networking, loader hand-over) or the real chip.

## Before you flash

- **Back the unit up.** At least the CAL and rootfs partitions; `0xFFFF` can read partitions (`0xFFFF -h`). The
  scripts here write only the rootfs partition and leave NOLO, the initfs and the CAL alone.
- **The unit needs the loader already.** The scripts assume a unit whose kernel partition holds
  `kernel/out/zImage-loader`.
- **Unplug the charger**, unless you mean to charge under this system.
- Keep a way back: another good rootfs image, or the backup.

## Flash

```sh
# copy the image and helper scripts to ~/n810/mbx-test on the jig host (prints sha256 sums at both ends: compare them)
JIG_HOST=user@jig-host scripts/device/stage-on-jig.sh

# start the flasher, then reboot the unit (power menu, or `reboot` over ssh): NOLO takes the image over USB
ssh user@jig-host 'cd ~/n810/mbx-test && sh flash-mbx.sh'      # 0xFFFF -m rootfs:rootfs-device.jffs2 -f -r
```

After the flash the unit reboots by itself. Log in with `ssh root@192.168.2.15` (password `n810`) over USB
networking. Weston starts on the panel at boot. If `dmesg | grep mbx` is empty, `modprobe mbx` loads the GPU driver.
`gles2_feat` runs the feature tests on the unit's own chip.

## If it does not boot

- If `/boot/zImage` does not boot, the loader falls back to `/boot/zImage.old`.
- If the rootfs itself is wrong, flash a good image with the same steps: the flasher window is there whenever the unit
  reboots into NOLO.

## What this guide does not cover

- **Putting the loader into the kernel partition.** With `0xFFFF` it is a `-m kernel:...` flash (read `0xFFFF -h`),
  and the unit then depends on the loader and on whatever `zImage.old` is until you have a rootfs you trust. Not
  scripted, not described step by step here.
- **N800.** The N800 kernel and loader are built, not run on one.
- **Other chargers or batteries.**
- **A clean-checkout run of these exact steps.** This page was assembled from the README and the scripts
  (`rootfs/scripts/build-rootfs.sh`, `stage-boot.sh`, `fetch-device-kernel.sh`, `scripts/device/flash-mbx.sh`); the
  build and flash path has been used during development, but the commands here have not been replayed from a fresh
  clone for this page.
