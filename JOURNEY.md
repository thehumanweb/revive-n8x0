# Journey: a modern GPU stack for the N810's PowerVR MBX

The work to give the MBX a from-scratch DRM kernel driver and a Mesa Gallium
GLES2 driver, aiming at an animated triangle on omapdrm: first in QEMU, then on
a real N810. This file is the state of play (sections 1-5) and the log of how
we got there (section 6). For the setup snapshot see `NOTES.md`.

**Sources.** Each section ends with where its facts came from, so they can be
checked: repo paths (relative to the repo root), files in the Mesa 26.1.8 and
Linux 6.18.58 trees (under `build/` and `kernel/build/`), URLs, and "user" for a
decision or instruction given in the session. A statement with no source is mine
and unverified. Everything the emulator says about the chip is the model's,
not the chip's: `mbx/README.md` marks which of its assumptions were observed on
a device.

## 1. Where we are (2026-11-06)

| Step | State |
|---|---|
| M0: test loop | Done. `scripts/run-qemu.sh` boots the guest; the DT node, config and rootfs wiring are in; the driver probes and reads `CORE_ID` 0x01010003. QEMU is built from this repo. |
| M1: kernel driver | Mostly done: probe and power-up, IRQ, per-file MBX address spaces with page tables, shmem GEM buffers and PRIME import code, a blocking submit that feeds the TA stream by CPU PIO. Missing: asynchronous submit with fences, DMA feeding, parameter-buffer and free-list setup, partial render on overflow, runtime PM. |
| M2: triangle without Mesa | Done in QEMU: the host `mbxtri` and the guest `mbxtri-drm` (through the kernel driver) draw byte-identical pictures. Not done: showing it on the panel. |
| M3: Gallium driver | In progress. A GLES2 triangle (EGL, an FBO, GLSL with a uniform matrix and a varying colour) renders on the host sim and is byte-identical to the emulator-only picture at four angles. Done: the vertex compiler (NIR to VGP, scalar), the fragment matcher for varying and constant colour, the context, the sim. Missing: richer fragment shaders (textures, fog, blending, alpha test), scissor, scanout through kmsro, the CPU fallback, uniforms in constant buffers beyond what the matcher handles, the shaders' trigonometry. |
| M4: animated triangle in the guest | Not started (needs Mesa cross-built through Buildroot, EGL/GBM, omapdrm flips). |
| M5: real N810 | Not started. See section 4 for what is unverified. |

Everything built here ran on the emulator only; the one hardware fact so far is the user's N810 probe in section 4. Commits are on `main`; the last
push was `d0b2ea1`, later commits are local until pushed.

Sources: this file's own log (section 6); `git log`; the plan and decisions:
user.

## 2. Decisions

- **Kernel driver:** a new DRM render node (`mbx/drm/`, GPL-2.0), written from
  scratch. The Imagination DDK shim (`mbx/kmod/`, `mbxaccess`) is not needed:
  no Nokia userland compatibility is wanted. It stays as a hardware reference;
  the rootfs no longer loads it (the two would claim the same MMIO).
- **Userspace:** a Gallium driver in Mesa (`mesa/src/gallium/drivers/mbx`,
  MIT) with a DRM winsys, and kmsro for omapdrm scanout. For the host, a
  drm-shim backend runs the emulator behind the same ioctls, so the real driver
  and winsys run without a kernel.
- **Display:** omapdrm with fbdev emulation (`/dev/fb0`; omapfb is gone from
  6.18).
- **Hardware shape:** the VGP does vertex work (vec4, no branches, 256
  instructions, 192 constants, 14 temporaries); the pixel side is fixed-function
  (two texture units with a word-C combiner, offset colour, fog, blending; no
  stencil in the model). Vertex shaders compile from NIR to VGP; fragment
  shaders are matched onto the fixed pipe.
- **Unsupported fragment shaders fall back to the CPU.** v1: the whole draw in
  software (Mesa `draw` plus a softpipe-style rasteriser on the mapped
  buffers). v2: hybrid, the VGP transforms and streams out and the CPU shades
  (needs VGP stream-out verified on the device).
- **Submit path:** the TA stream goes to the TA port first by CPU PIO, then by
  omap-dma (fixed destination, 32-bit); DMA into the MBX is possible (user).
  The port stalls the bus while its 59-word FIFO is full, so unpaced DMA
  works in the model.
- **Memory:** the MBX has its own MMU, so buffers are scattered shmem pages
  mapped into a per-file 32 MB address space. Scanout buffers come from the
  KMS device (omapdrm dumb buffers, contiguous) and are imported by PRIME.
- **Mesa:** fetch it; use the version Buildroot 2026.08.1 ships (26.1.8).
- **Constraints:** work only inside this repo; JOURNEY.md cites sources; no
  golden captures of the real stack exist, so the emulator is the only
  reference (user).

Sources: user (all of the above that is a decision); the model facts: `mbx/README.md`,
`mbx/emu.h`, `mbx/vgp.c`, `mbx/core.c` (the 59-word FIFO and the stalling port
in README "Known gaps"); `NOTES.md` (omapdrm and the NOLO limit).

## 3. What exists, and how to run it

| Piece | Where | Notes |
|---|---|---|
| Emulator core | `mbx/*.c`, `mbx/mbxemu.h` | No QEMU dependency; `make -C mbx` makes `libmbxemu.a`. |
| Kernel driver | `mbx/drm/` | `mbx_drv.c` (probe, power, IRQ, ioctl table), `mbx_mmu.c`, `mbx_gem.c`, `mbx_job.c`, uapi `mbx_drm.h`. Kernel patches `0063` (Kconfig/Makefile hook) and `0064` (`gpu@50000000` in `omap2420.dtsi`). `CONFIG_DRM_MBX=m`: the kernel image is 14 KB under the 2 MB NOLO limit, so new kernel code must be modules. |
| VGP encoder | `mbx/lib/mbx_vgp.h`, `mbx/tests/t_vgp.c` | Header-only encode/decode/disassemble with the hardware's operand rules; `make -C mbx test`. |
| Test programs | `mbx/tools/` | `mbxtri` (host emulator), `mbxtri-drm` (render node), shared stream builder `tri_frame.h`. |
| Mesa overlay | `mesa/` | `docker/` (host build image), `patches/` (hooks: 0001 meson, loaders and targets; 0002 kmsro), `src/` (driver and winsys sources, the drm-shim), `scripts/`. |
| Boot script | `scripts/run-qemu.sh` | Builds the OneNAND flash image and boots the `n810` machine; defaults to the QEMU built from this repo. |

Commands, from the repo root (long ones in the background; a foreground
timeout once killed `make` mid-write and left corrupt objects):

- QEMU from this repo: `qemu/prepare-tree.sh $PWD/build/qemu-src --mbx`, then
  the docker build in `NOTES.md` (Rebuild) into `build/qemu-src/build/`.
- Kernel: `kernel/scripts/prepare-6.18.sh`, `kernel/scripts/build-6.18.sh`;
  module only: `kernel/scripts/build-mbx-module.sh`.
- Rootfs (modules, tools, image): `rootfs/scripts/build-rootfs.sh`.
- Boot: `scripts/run-qemu.sh` (console on tcp 5577, user `root`, no
  password). The guest login and test run are not scripted in the repo yet.
- Host test programs: `make -C mbx mbxtri test`.
- Mesa host build: `docker build -t n810-mesa:1 mesa/docker`;
  `mesa/scripts/prepare-mesa.sh [--sync]`; `mesa/scripts/build-host.sh`
  (first time) or `mesa/scripts/ninja-host.sh` (incremental); the shim is
  `build/mesa-host-install/lib/x86_64-linux-gnu/libmbx_sim_drm_shim.so`, used
  with `LD_PRELOAD`.

Sources: the scripts themselves; `NOTES.md` (rebuild recipes and boot);
`kernel/scripts/build-6.18.sh` (the 2 MB check).

## 4. What has been checked, and what has not

Checked, on the emulator:

- The host `mbxtri` renders a Gouraud triangle (1 scene, 1 primitive, 16 tiles,
  no stream errors, 1144 of 4096 pixels, correct winding and colours).
- In the QEMU guest (QEMU built from this repo), `mbxtri-drm` through the
  kernel driver gives a picture byte-identical to the host one (12,301 bytes).
- The same program on the host drm-shim gives the same picture again.
- `t_vgp`: 128,034 randomised checks of the encoder against the model's own
  `vgp_exec` (not covered: LIT, EXPP, LOGP).
- Mesa 26.1.8 builds with the mbx driver; libgallium carries it.
- `mesa/scripts/test-sim.sh`: the GLES2 triangle on the host sim (real EGL, GLSL
  compiler, NIR, the mbx compiler and context, the DRM winsys, the shim and the
  emulator) equals `mbx/mbxtri`'s picture byte for byte at 0, 30, 90 and 200
  degrees. It uses a uniform `mat4`, a varying colour, a depth buffer and a
  float vertex array.

Checked on a real N810 (a probe the user ran on the device and reported in the
session; I have not run anything on a device):

- **The TA needs bit 17 of the sections word**: the word is `0xf0020000 | mask`, not
  `0xf0000000 | mask`. With that bit cleared at emission, the first frame never
  rendered: the TA took nothing (`INT_STATUS 003b0001`: the input FIFO full, 59 words
  in bits 16-23, and bit 0 set), the frame was dropped, and the render was not done
  after a second; no frame was captured. The baseline run with the bit set had 0
  failures. What the bit selects is not known. The other probes of that batch were
  not run.

Sources: the user's probe on the device (reported in the session).

Not checked, or only the model's word for it:

- **Anything on a real N810.** The emulator is the only reference.
- The parameter buffer and free list: the model's writer is off without a
  free list; the real chip needs one. `0x85c`'s other bits (the model reads only
  the tile grid and bit 7); the ISP/TSP state bits the chip needs beyond Gouraud
  and depth; whether the chip wants more header blocks than the model's minimum.
- That the MMU page-table entries are bare physical addresses and that it
  confines the MBX (README assumption 8 and `core.c` are the model's).
- SDMA to the TA port; VGP stream-out addressing (README assumption 23: a
  fixed, alternating X/Y place, the address word ignored).
- The cull-bit mapping (README assumption 4 combined with the viewport's y
  sign): not yet compared against a picture.
- That a CPU-filled colour clear survives a render over untouched tiles: it
  relies on the model reading the target's own pixels when no background
  object is set (README assumption 6).
- PRIME import of omapdrm scanout buffers and the kmsro path: written, never run.
- Mesa draws beyond that one shape: only varying and constant colour fragment shaders are matched; indexed draws, strips, fans, other vertex formats, blending and textures have not been run.

Sources: `mbx/README.md` (assumptions 4, 5, 6, 7, 8, 13, 23 and "Known gaps");
`mbx/core.c`, `mbx/pb.c`, `mbx/ta.c` (`TA_PRIM_SECTIONS`), `mbx/render.c`
(`render_kick`); the results are from the runs described in section 6.

## 5. Next

1. Fragment shaders on the fixed pipe: texture times colour, constants through the
   word-C combiner, fog, alpha test from `discard`, blend and depth from state;
   what does not reduce falls back to the CPU.
2. More of the vertex side: indirect uniform reads (AMV and `c[a0+n]`), a source
   swizzle optimisation (DP4 on whole vec4 operands instead of scalar MADs: the
   matrix transform is 23 instructions now where four DP4s would do), LIT, trig
   approximations, more than 128 constants (a constant window).
3. Scissor as user clip planes; points and lines; textures and their formats.
4. Mesa cross-built through Buildroot, EGL/GBM and omapdrm flips for the animated
   triangle (M4), then the real device (M5).

Sources: the VGP's constraints: `mbx/vgp.c`, `mbx/ta.c` (`vertex()`), `mbx/lib/mbx_vgp.h`; the instruction count of the
matrix transform: the disassembly `MBX_DEBUG=2` prints.

## 6. Log

### 2026-11-06: plan and M0

- Read `NOTES.md`, the emulator and the old kmod. The old driver marshals
  Imagination "services" calls for a closed userland and has no GEM, scheduler,
  fences or DT node.
- Plan agreed (M0 to M5, section 1). The 6.18 tree had no MBX DT node (only
  `gfx_clkdm`); `DRM_OMAP=y`; `zImage-6.18` was 2,081,046 bytes against the 2 MB
  NOLO limit, so the driver and `drm_gem_shmem_helper` had to be modules.
- Wrote `scripts/run-qemu.sh` (flash image plus QEMU, from the NOTES recipe).
- Patches: `0063` hooks `drivers/gpu/drm/mbx` into the DRM Kconfig and Makefile;
  `0064` adds `gpu@50000000` to `omap2420.dtsi` (INTC 21; clocks `gfx_ick`,
  `gfx_2d_fck`, `gfx_3d_fck`). `prepare-6.18.sh` copies `mbx/drm/` into the tree.
  `CONFIG_DRM_MBX=m` sticks and pulls in `CONFIG_DRM_GEM_SHMEM_HELPER=m`.
- Driver skeleton: platform probe, the GFX power-up and reset sequence taken from
  the old driver (clocks on, L3/2 rates, power domain on, reset pulse; the order
  matters or the interconnect aborts every later access), the IRQ, a render node
  with `GET_PARAM`.
- Lesson: `build-6.18.sh` takes over two minutes; a foreground timeout killed
  `make` and left zero-size objects and missing `.o.d` files. Delete them,
  rebuild, run builds in the background.
- Rebuilt: `zImage-6.18` is 2,082,918 bytes (14,234 under the limit); `mbx.ko`
  and `drm_shmem_helper.ko` are in the modules tree. The rootfs overlay loads
  `mbx` (`S35mbx`) and no longer stages `mbxaccess.ko`.
- **First light in QEMU:** the guest boots, `mbx` probes, the log has `mbx
  50000000.gpu: MBX core id 0x01010003, revision 0x5010307` and `[drm]
  Initialized mbx 1.0.0 ... on minor 1`, and `/dev/dri/renderD129` exists next to
  omapdrm's `card0`/`renderD128`. The model's revision register (0xf10) is not
  zero; `NOTES.md` had only noted the product-ID registers at 0xfe0-0xffc
  reading 0.
- Sources: `NOTES.md`; `mbx/README.md`, `mbx/{emu.h,mbxemu.h,vgp.c,ta.c}`;
  `mbx/kmod/gpl/driver/mbxaccess.c` (the ioctls, and the power-up sequence in
  `MBXAccess_GfxClocks`, about lines 820-900); kernel tree
  `kernel/build/linux-6.18.58`: `arch/arm/boot/dts/ti/omap/omap2.dtsi` (the `ocp`
  bus), `omap2420.dtsi`, `omap24xx-clocks.dtsi` (labels `gfx_ick`, `gfx_2d_fck`,
  `gfx_3d_fck`), `omap2420-clocks.dtsi` (`gfx_clkdm`), `drivers/gpu/drm/{Kconfig,Makefile}`,
  `include/drm/{drm_gem_shmem_helper.h,drm_drv.h}`, `drivers/gpu/drm/drm_gem_shmem_helper.c`,
  the generated `build/obj-6.18.58/.config`; `kernel/scripts/{build-6.18.sh,prepare-6.18.sh}`;
  decisions: user.

### 2026-11-06: M2 step 1, `mbx/tools/mbxtri.c`

Work is confined to this repo from here on (the user asked); everything below
comes from `mbx/*.c` and its README.

- `mbxtri` drives `libmbxemu` on the host the way a driver drives a frame. `-a DEG`
  rotates the triangle through the VGP constants (the image changes).
- The sequence that works, from the emulator source:
  1. TA setup: `0x85c` config = `0x80 | (tiles_x-1) << 20 | (tiles_y-1) << 10`
     (bit 7 makes the end word end the scene), `0x84c`/`0x850` size << 16, `0x844`
     region array, `0x840` tails, `0x810` context 0, `0x800` start.
  2. Stream: object header `0x10000000 | mask` with blocks in bit order (state,
     alpha test, layout, viewport x-scale/x-off/y-off/y-scale, depth
     scale/offset/1e-5); a VGP section `0x80000000 | bit`, a flags/range word, a
     code header `0x90000000 | first << 16 | last` and two words an instruction,
     constants `0xa0000000 | first << 16 | last` and four floats each; the
     primitive word (`0x70000000`: triangle list), the sections word (`0xf0020000 | mask`; the chip needs bit 17, observed on a real N810 by the user's probe), vertices as
     the layout names them, TA control port 1 (object ends), `0xf0000000` (scene
     ends).
  3. Render: `0x608` regions, `0x610` = `0x30000` (depth on chip), `0x61c`, `0x650`
     format (0xe ARGB8888), `0x654`/`0x658` (size-1) << 16, `0x65c` address, `0x660`
     stride in words, `0x83c` background object (zero: the target's own pixels),
     `0x6ec` context, `0x680` kick.
- VGP facts used: `o1` is BGRA (a `.zyxw` swizzle brings packed RGBA right), missing
  input components are 0 with w = 1, DP4 writes a scalar to the lanes of its mask
  (a 4x4 transform is four DP4s with masks X, Y, Z, W against c0-c3), and the TA
  does the divide and viewport so `o0` stays clip space.
- Sources: all from this repo's model. `mbx/emu.h` (`R_*`, `I_*`, `TAC_*`);
  `mbx/core.c` (`mbxemu_reg_write`: what each write does, the MMU, contexts, free
  list); `mbx/ta.c` (`command()`, `hdr_words`, `hdr_block()`, the section, code
  and constant words in `ta_word()`, `vertex_words()`, `vertex()`, `project()` for
  the viewport and depth formulas, `ta_start()`, `ta_end_scene()`,
  `write_regions()`, `tile_grid()`); `mbx/vgp.c` (instruction layout in its header
  comment); `mbx/render.c` (`render_kick()`, `depth_pass()`, the state-word bits at
  the top); `mbx/pb.c` (the parameter-buffer writer is off without a free list);
  "o1 is BGRA": `mbx/ta.c` `project()` and README assumption 12.

### 2026-11-06: M2 step 2, the kernel path (`mbxtri-drm`)

- `mbx/drm` grew from a probe into a render node that can run a frame:
  - `mbx_mmu.c`: an address space a DRM file, 32 MB of 4 KB pages through the eight
    page tables (bare physical page addresses), allocated coherent; pages with
    no buffer point at a scratch page, never physical 0; `drm_mm` places buffers
    from 64 KB up. The MMU holds one space at a time; a job switches it (writing
    the tables flushes the TLB).
  - `mbx_gem.c`: shmem GEM objects mapped write-combined for the CPU (`BO_CREATE`);
    `BO_INFO` maps one into the file's MBX space and returns the device address
    and mmap offset; `BO_UNMAP`. PRIME import goes through
    `drm_gem_shmem_prime_import_sg_table` (untested).
  - `mbx_job.c`: `SUBMIT` is one blocking frame: TA registers, `0x800` start, the
    stream as chunks (type << 28 | count; TA words to the TA port, control words
    to the control port, by CPU PIO), wait for TA_COMPLETE, render registers,
    kick, wait for RENDER_COMPLETE. The interrupt handler clears the bits and
    wakes the waiter; TA_OFLOW and TA_STREAM end a wait as -ENOMEM and -EINVAL; a
    timeout or error resets the cores (`0x080` = `0x111`). One frame at a time.
- Shared test code `mbx/tools/tri_frame.h` builds the stream for both `mbxtri` and
  `mbxtri-drm`, so both send the same words. Tooling: `kernel/scripts/build-mbx-module.sh`
  (module-only rebuild, about a minute), `rootfs/scripts/stage-tools.sh` (cross-builds
  `mbxtri-drm` with Buildroot's host gcc into a rootfs overlay).
- **Result:** in the QEMU guest `mbxtri-drm` found `/dev/dri/renderD129`, made four
  buffers, submitted and drew 1144 of 4096 pixels. With QEMU built from this repo
  the guest's picture is byte-identical to the host's.
- A first run used a QEMU binary from outside the repo and differed in 8 bytes (one
  colour step at four mirror-image pixel pairs): that binary embeds a different
  emulator core. Since then `scripts/run-qemu.sh` defaults to the in-repo build and
  refuses to start without one.
- Sources: page-table format, space size, TLB behaviour: `mbx/core.c` (`translate()`,
  `R_MMU_*` writes) and README assumption 8; interrupt bits and soft reset
  (`0x080`, bits 0, 4, 8): `mbx/core.c` (`soft_reset`, `emu_raise`); the power-up
  sequence: `mbx/kmod/gpl/driver/mbxaccess.c` (`MBXAccess_GfxClocks`); kernel APIs
  read in `kernel/build/linux-6.18.58`: `include/drm/drm_gem_shmem_helper.h`,
  `drivers/gpu/drm/drm_gem_shmem_helper.c` (`drm_gem_shmem_get_pages_sgt`,
  `drm_gem_shmem_prime_import_sg_table`, `map_wc`), `include/drm/drm_drv.h`
  (`open`, `postclose`, `gem_prime_import_sg_table`), `include/drm/drm_mm.h`; QEMU
  is built from `qemu/` (`qemu/prepare-tree.sh --mbx`, `qemu/docker/Dockerfile.9.1`).

### 2026-11-06: the VGP encoder (`mbx/lib/mbx_vgp.h`)

- A header-only encoder, decoder and disassembler with the hardware's rules built
  in (one constant register and one input register an instruction, sources from
  src0, 192 constants, temporaries 0-13, outputs 0-7, register file 3 refused).
  Errors come back as `MBX_VGP_E_*`, so the compiler can say which rule a shader broke.
- `make -C mbx test` (`tests/t_vgp.c`) runs every legal instruction it can generate
  (MOV, ADD, MUL, MAD, DP3, DP4, MIN, MAX, SLT, SGE, FRC, RCP, RSQ, EXP, LOG, DST;
  random registers, swizzles, negates, masks, constant windows, relative constants)
  through the model's `vgp_exec()` and compares with a reference built from the
  operand description, so a field in the wrong bit would show. 128,034 checks, no
  failures; also the refusals, the disassembly, AMV and `c[a0+n]` with a window.
- Sources: layout, opcode list and numbering, the register rules, windows,
  relative constants and AMV: `mbx/vgp.c` (header comment, `enum`, `nsrc[]`,
  `read_sources()`, `vgp_exec()`, `vgp_run()`) and README assumption 13; output
  roles: `mbx/ta.c` `vertex()` (o1 colour BGRA, o2 offset colour with fog in w, o3
  and o4 texture coordinates, o5/o6 user clip planes enabled by sections-word bits
  20-27, o7.x and o7.y front and rear clip, always on) and `project()`; limits:
  `mbx/emu.h` (`struct vgp`, `struct vgp_regs`).

### 2026-11-06: Mesa 26.1.8 builds with an mbx driver (`mesa/`)

- Buildroot 2026.08.1 ships Mesa 26.1.8; `mesa/scripts/prepare-mesa.sh` fetches it
  (sha256 checked), applies `mesa/patches/` and copies in `mesa/src` and the uapi
  header. `mesa/docker` builds the host image (`n810-mesa:1`);
  `mesa/scripts/build-host.sh` configures and builds Mesa with only the mbx gallium
  driver, GLES2 and EGL (GBM, surfaceless), no LLVM.
- Patch 0001 hooks `mbx` into meson (options, default driver lists,
  `with_gallium_drm`), the DRM pipe loader, `drm_helper` and both targets. Patch
  0002 adds an `mbx` branch to kmsro: the KMS device (omapdrm) allocates the
  scanout buffer as a dumb buffer and the MBX imports it by PRIME.
- A screen with the VGP's limits as caps (256 instructions, 192 constants, 14
  temporaries, no control flow, two samplers) and a winsys that recognises the
  render node by `MBX_ABI_VERSION` (moved into the uapi header so kernel and Mesa
  share it).
- 26.1 differences from older Mesa: caps are a `pipe_caps` struct filled at screen
  creation (`u_init_pipe_screen_caps`) and `shader_caps[]`, not `get_param`; uapi
  headers are in `include/drm-uapi`; `-Dgallium-vdpau` is gone (`-Dvideo-codecs=`).
- Sources: the version, URL and sha256: `rootfs/build/buildroot-2026.08.1/package/mesa3d/{mesa3d.mk,mesa3d.hash}`
  (the hash file cites the announcement
  https://lists.freedesktop.org/archives/mesa-announce/2026-August/000865.html; tarball
  from https://archive.mesa3d.org/mesa-26.1.8.tar.xz). Hook points found by searching the
  26.1.8 tree for how `vc4` is wired in: `meson.options`, `meson.build`,
  `src/gallium/meson.build`, `src/gallium/auxiliary/pipe-loader/{pipe_loader_drm.c,meson.build}`,
  `src/gallium/auxiliary/target-helpers/{drm_helper.h,drm_helper_public.h}`,
  `src/gallium/targets/{dri,dril}/*`, `src/gallium/winsys/vc4/drm/*`,
  `src/gallium/winsys/kmsro/drm/kmsro_drm_winsys.c`. Screen and caps API:
  `src/gallium/drivers/vc4/vc4_screen.c` (`vc4_init_screen_caps`, `vc4_init_shader_caps`,
  `vc4_screen_create`). Docker image: `mesa/docker/Dockerfile` (Ubuntu 24.04, pip meson);
  Mesa requires `meson_version : '>= 1.4.0'` (`meson.build`).

### 2026-11-06: the host sim (drm-shim)

- `mesa/src/gallium/drivers/mbx/drm-shim/mbx_sim.c` is a Mesa drm-shim backend that
  implements the five mbx ioctls in userspace on top of the emulator core (copied in
  by `prepare-mesa.sh`), with the kernel driver's layout: per-file page tables in the
  shim's memfd (physical memory), buffers mapped with a VMA heap, a submit that sets
  the MMU and the TA, feeds the stream and renders. `LD_PRELOAD=libmbx_sim_drm_shim.so`
  makes a program see an `mbx` render node, so the real DRM winsys and driver run on
  the host with no kernel or QEMU. Mesa builds it with `-Dtools=drm-shim`.
- Check: `mbxtri-drm` built for x86 and run on the shim draws the same picture as
  the host emulator (byte-identical PPM).
- Sources: Mesa 26.1.8 `src/drm-shim/{README.md,drm_shim.h,device.c}` (the shim API,
  the 4 GB memfd and its heap, driver ioctl indexing at `device.c` ~307,
  `drm_shim_driver_prefers_first_render_node` in `drm_shim.c` ~120) and
  `src/broadcom/drm-shim/{vc4_noop.c,meson.build}` as the pattern; `src/meson.build`
  (`subdir('drm-shim')` comes before gallium); the ioctl semantics are this repo's
  `mbx/drm/mbx_job.c` and `mbx_mmu.c`.

### 2026-11-06: the mbx pipe_context (resources, state, jobs, draw)

- The driver now has a context that records and submits frames: `mbx_bo.c` (ioctl
  wrappers), `mbx_resource.c` (buffers, linear 2D targets padded to whole 16-pixel
  tiles, depth buffers with no memory because depth is on chip), `mbx_state.c` (state
  objects as copies), `mbx_job.c` (the TA stream as chunks, one `SUBMIT` per flush;
  regions, tails and background buffers kept per target size), `mbx_draw.c` (vertices
  read on the CPU into the stream after an object header; the VGP program and
  constants only when they changed in the job; triangle lists, strips, fans),
  `mbx_context.c` (flush, a CPU-filled colour clear, depth clear as the next render's
  on-chip value). It builds with the rest of Mesa on the host.
- Not there yet: the compiler and matcher (a draw logs "no program" and skips),
  textures, blend and alpha-test words, scissor (planned as user clip planes in the
  vertex program), scanout through kmsro.
- Cull bits follow README assumption 4 (bit 19 culls negative area in surface pixels,
  bit 18 positive) combined with the sign of the viewport's y scale; untested.
- 26.1 API changes met: `clear` takes `color_clear_mask` and `stencil_clear_mask`;
  `set_constant_buffer` and `set_sampler_views` have no ownership flag (the driver
  takes its own reference); `util_set_vertex_buffers_count` takes four arguments; the
  stage type is `mesa_shader_stage`.
- Sources: callback set and structure: Mesa 26.1.8 `src/gallium/drivers/vc4/{vc4_context.c
  (vc4_context_create),vc4_state.c (vc4_state_init),vc4_draw.c,vc4_program.c,vc4_resource.c}`;
  current signatures: `src/gallium/include/pipe/p_context.h` (clear, set_constant_buffer,
  set_sampler_views, set_vertex_buffers, the sampler view release comment),
  `src/gallium/auxiliary/util/u_helpers.{h,c}`, `src/util/u_pack_color.h`,
  `src/gallium/auxiliary/util/u_debug_cb.h`; the compile errors that showed them were in
  `build/mesa-host.log`. TA stream, header blocks, state-word bits, region array size,
  `0x6dc` and the render registers: `mbx/ta.c`, `mbx/render.c` (`render_kick`,
  `depth_pass`), `mbx/emu.h`, README assumptions 4, 5, 6, 7.

### 2026-11-06: a GLES2 triangle on the host sim

- Mesa found the mbx driver through the shim ("using driver mbx") and EGL came up
  once the screen set `blend_equation_separate` (GLES 2.0 needs
  `ARB_vertex_shader`, `ARB_fragment_shader`, `ARB_texture_non_power_of_two`
  and `EXT_blend_equation_separate`; the rest come from `u_init_pipe_screen_caps`).
- The vertex compiler (`mbx_compile.c`, `mbx_nir.c`): NIR is prepared as vc4
  does (`nir_lower_io` in vec4 slots, scalar ALU, constants folded, loops
  unrolled, booleans as floats). Pass one turns scalar SSA into operations on
  virtual scalar registers; pass two gives them lanes of r0-r12 (r13 stages
  `gl_Position`), obeys the one-constant and one-input register rule by
  copying, and encodes with `mbx_vgp.h`. Any scalar operation can run in any
  lane (the source swizzle picks the lane, the write mask the lane written),
  so one allocator over 13 x 4 lanes suffices. `o0` is the position, `o7.x =
  z + w` and `o7.y = w - z` give the near and far clip the hardware lacks, `o1`
  takes the colour in BGRA order.
- The fragment matcher (`mbx_fs_match`): `gl_FragColor` made of one varying's
  components in order and constants; Gouraud unless declared flat or all
  constant. Anything else logs that a CPU fallback is planned and skips the draw.
- The test (`mesa/tests/gles2_tri.c`, `mesa/scripts/test-sim.sh`) matches the
  emulator-only picture at four angles, byte for byte.
- Two bugs worth remembering, found with the debug aids (`MBX_DEBUG=1|2`,
  `MBX_SIM_TRACE=1|2`, which turns on the emulator's own trace):
  - `nir_intrinsic_component()` must not be called on `load_uniform`: it has no
    such index and the accessor read another field (4), so every constant lane
    was 4-7; the symptom was a degenerate triangle ("TA end: 0 triangles"). Found by
    printing the raw instruction words and checking the encoder in a tiny standalone test.
  - `index_bias` is only defined for indexed draws; on a plain `glDrawArrays` it
    held garbage and the vertex gather read far outside the buffer (all zeros).
- Sources: Mesa 26.1.8 `src/mesa/main/version.c` (`compute_version_es2`),
  `src/mesa/state_tracker/st_extensions.c` (`EXT_CAP`, the GLSL level),
  `src/gallium/auxiliary/util/u_screen.c` (cap defaults), `src/compiler/nir/nir.h`
  (the `nir_scalar` helpers, `nir_intrinsic_*` accessors),
  `src/compiler/nir/nir_lower_bool_to_float.c` (which ops it produces),
  `src/compiler/nir/nir_shader_compiler_options.h` (the option names),
  `src/gallium/drivers/vc4/vc4_program.c` (`vc4_shader_state_create`,
  `vc4_optimize_nir`, the options block) as the pattern for the NIR pipeline;
  hardware rules and the clip outputs: `mbx/vgp.c`, `mbx/ta.c` (`vertex()`),
  `mbx/README.md` assumptions 13 and 14; the comparison picture: `mbx/mbxtri`.
