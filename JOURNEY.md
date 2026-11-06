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
| M1: kernel driver | Mostly done: probe and power-up, IRQ, per-file MBX address spaces with page tables, shmem GEM buffers and PRIME import code, an asynchronous submit (ordered workqueue, a `dma_fence` per job, implicit sync through `dma_resv`, `WAIT_FENCE`) that feeds the TA stream by CPU PIO, and the parameter buffer with its free list (a 4 MB object at the top of every address space, the list built once after each reset). Missing: DMA feeding, partial render on overflow (an overflow is an error for now), runtime PM. |
| M2: triangle without Mesa | Done in QEMU: the host `mbxtri` and the guest `mbxtri-drm` (through the kernel driver) draw byte-identical pictures. Not done: showing it on the panel. |
| M3: Gallium driver | In progress. A GLES2 triangle (EGL, an FBO, GLSL with a uniform matrix and a varying colour) renders on the host sim and is byte-identical to the emulator-only picture at four angles. Done: the vertex compiler (NIR to VGP, scalar), the fragment matcher for varying and constant colour, the context, the sim. Missing: richer fragment shaders (textures, fog, blending, alpha test), scissor, scanout through kmsro, the CPU fallback, uniforms in constant buffers beyond what the matcher handles, the shaders' trigonometry. |
| M4: animated triangle in the guest | **Done in QEMU.** `gbm_tri` (EGL on GBM, KMS on omapdrm through kmsro, GLES2 through the mbx driver, the model rendering) shows the rotating RGB triangle on the guest's 800x480 panel: two screendumps six seconds apart show it at different angles (`docs/gbm_tri-panel-a.png`, `-b.png`). Not yet: a frame rate, vsync behaviour, the real display.
| M5: real N810 | **In progress.** The device boots our rootfs and kernel (stock initfs, loader, kexec); `mbx.ko` probes (`CORE_ID 0x01010003`, revision `0x05010307`, as modelled) and the kernel driver now does the contract (below); the TA bins the triangle as the model describes (lists and object block in memory check out). The picture is still noise: any tile the render processes comes out as noise, and the cause is not found (section 6, last entry). |

Everything built here ran on the emulator only (QEMU's n810 with the model, or the model on the host); the one hardware fact so far is the user's N810 probe in section 4. Commits are on `main`; pushed up to `6e8e5dd`, later commits are local until pushed.

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
- Rootfs (modules, tools, Mesa with mbx, the demos, image): `rootfs/scripts/build-rootfs.sh`
  (prepares and copies Mesa, then Buildroot builds it from `rootfs/build/mesa-src`; a Mesa-only change rebuilds in minutes).
- Boot: `scripts/run-qemu.sh` (console on tcp 5577, user `root`, no
  password); `scripts/guest.py PORT CMD...` logs in and runs commands, `scripts/panel-shot.sh OUTDIR "CMD"` runs one in the guest and takes QEMU screendumps of the panel (`scripts/ppm2png.py` makes PNGs).
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
- In the QEMU guest, `gbm_tri` runs on the display: the mode set and page flips are accepted
  and two screendumps of the panel show the triangle rotated differently (83,950 pixels differ);
  the background is the clear colour as the panel's RGB565 shows it (24,24,32). No frame rate
  was measured (QEMU's speed says nothing about the device's).
- `t_pb`: the free list through the MMU as the kernel driver sets it up (9 checks): the list
  builds, the TA bins into the parameter buffer, the render equals the one without it, 40
  frames reuse one list (a render gives its pages back), and a two-page list overflows.
  In the QEMU guest the register trace shows the kernel building the list once and then
  179 TA starts with no overflow, and `gbm_tri` still animates on the panel.
- `t_ta`: the model reproduces the hang above (15 checks): a sections word without bit 17
  stops the TA, the FIFO fills to 59, no `TA_COMPLETE` and no render completion, and a TA
  start or a soft reset recovers; with the bit set a frame completes and draws.
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

- **Anything on a real N810 beyond the one observation above.** The emulator is the only
  reference. Bit 0 of `INT_STATUS`, set on the device when the TA hung, is not set by the
  model and its meaning is not known.
- The parameter buffer and free list on the chip: the kernel builds them as the model
  expects (the table and pages at fixed device addresses, `0x824`-`0x82c`, then `0x838`
  and a wait for `EVM_DALLOC`), and the model accepts it, but the chip's own protocol,
  the buffer's size, and what a real overflow looks like are not known. `0x85c`'s other
  bits (the model reads only the tile grid and bit 7); the ISP/TSP state bits the chip
  needs beyond Gouraud and depth; whether the chip wants more header blocks than the
  model's minimum.
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
- In the QEMU guest (ARM, the real kernel driver, Mesa cross-built by Buildroot), `gles2_tri` draws the same triangle (1144 of 4096 pixels; the PPM has not been compared byte for byte with the host's yet).
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
4. The real device (M5): the unverified list above is what to probe first.

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
     primitive word (`0x70000000`: triangle list), the sections word (`0xf0020000 | mask`; the chip needs bit 17, observed on a real N810, README assumption 30), vertices as
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

### 2026-11-06: scanout resources (written, untested)

- `mbx_resource.c` can now make buffers the display scans out: with a renderonly
  device (kmsro) a resource bound for scanout or display is allocated by the KMS
  device as a dumb buffer (padded to whole 16-pixel tiles) and imported by dma-buf
  into the MBX's address space through `BO_INFO`; `resource_get_handle` returns the
  KMS handle for the display side or a dma-buf fd, `resource_from_handle` imports,
  `resource_get_param` reports stride, offset and the linear modifier, and the
  screen answers the dma-buf modifier queries (linear only). The host sim has no
  display device, so this path first runs in the guest; the host triangle test is
  unchanged (identical at four angles).
- Sources: Mesa 26.1.8 `src/gallium/drivers/lima/lima_resource.c`
  (`lima_resource_create_scanout`, `lima_resource_get_handle`, `lima_resource_get_param`)
  as the render-only-GPU pattern; `src/gallium/auxiliary/renderonly/renderonly.{h,c}`;
  `src/gallium/include/pipe/p_screen.h` (the hook signatures);
  `src/gallium/drivers/vc4/vc4_screen.c` (the modifier query shape);
  `src/gallium/winsys/kmsro/drm/kmsro_drm_winsys.c` (why `renderonly_create_kms_dumb_buffer_for_resource`).

### 2026-11-06: Mesa and the demos in the guest

- `rootfs/` now builds Mesa for the guest: the defconfig selects `BR2_PACKAGE_MESA3D` with EGL, GLES and GBM and softpipe as the base driver Buildroot insists on (also a CPU reference); `external.mk` appends `mbx` to the package's driver list; `local.mk` points `MESA3D_OVERRIDE_SRCDIR` at our prepared tree (copied to `rootfs/build/mesa-src` because the build container sees only `rootfs/`); `package/mbx-demo` builds `gbm_tri` and `gles2_tri` from `mesa/tests`. `build-rootfs.sh` runs `prepare-mesa.sh`, copies the sources, and clears the override packages' stamps so each run re-syncs and rebuilds incrementally.
- Four build failures on the way, all in the glue: the container could not see the Mesa tree (path outside `rootfs/`); Buildroot's `mesa3d.mk` passes `-Dgallium-vc4-neon` on ARM without NEON, which Mesa 26.1.8 no longer defines (patch `0003` accepts and ignores it); Buildroot did not re-sync an override tree whose stamp existed (stamps are cleared now); then it built.
- **First guest run** (`dmesg`: `mbx` probes, `/dev/dri/card0` and `card1` and `renderD128`/`renderD129`):
  - `gles2_tri` (surfaceless EGL, offscreen): EGL lists pbuffer configs, `GL_RENDERER PowerVR MBX`, the program compiles to 31 instructions, one flush, and `1144 of 4096 pixels drawn`. The whole stack runs on the target OS.
  - `gbm_tri` (card0, omapdrm, through kmsro): the screen is created, EGL lists window configs for XRGB8888 and RGB565, 800x480, `GL_RENDERER PowerVR MBX`, the program is compiled, then a segmentation fault in the first draw, before the first flush. Debug lines for the scanout resource and the draw phases are being added to locate it.
  - dmesg also shows the panel's existing `RFBI: FRAMEDONE not received for 250ms` / "no tearing effect signal" notes when the display is first used; the QEMU panel model, not this work.
- Sources: Buildroot 2026.08.1 `package/mesa3d/{Config.in,mesa3d.mk}` (the option symbols, `MESA3D_GALLIUM_DRIVERS-y`, the vc4-neon line), `package/libdrm`; the build logs `build/rootfs-mesa.out` and `rootfs/build/build-rootfs.log`; the guest output `build/m4.out`; `mesa/tests/gbm_tri.c` follows the usual EGL-on-GBM and KMS flow with libdrm (`drmModeSetCrtc`, `drmModePageFlip`).
- **A mistake found and fixed here:** the repo's root `.gitignore` had `src/` (for the downloaded source tarballs), which also hid `mesa/src/`, so the Mesa driver's own sources (the screen, context, compiler, resources, winsys and the drm-shim: 21 files) were never committed. The commits up to `6e8e5dd` (pushed) and `0019729` therefore have the patches, scripts, tests and journal but not those sources, although their messages described them. `.gitignore` now has `!/mesa/src/` after `src/` and the files are in this commit.

### 2026-11-06: the model learns the sections word's bit 17

- The user's probe on a real N810 showed the TA takes nothing without bit 17 in the word
  after a primitive (section 4). The model now does the same: `mbx/ta.c` hangs the TA at
  such a word (`TA_HUNG`: later words wait in the input FIFO, `INT_STATUS` bits 16-23 count
  them up to 59, no `TA_COMPLETE`), `mbx/render.c` completes no render while it is hung, and
  a TA start or a soft reset ends it. `mbx/README.md` assumption 30 records it as observed;
  `mbx/tests/t_ta.c` (in `make -C mbx test`) checks a normal frame, a frame without bit 17,
  the FIFO's fill and its cap, and both ways out. The drivers already send the bit
  (`mbx/tools/tri_frame.h`, `mbx_program.c`), so the host triangle and the sim comparison
  are unchanged (identical at four angles).
- Sources: the user's probe on the device (reported in the session); the model's own code
  (`mbx/ta.c`, `mbx/core.c`, `mbx/render.c`, `mbx/emu.h`); `mbx/README.md` assumption 30.

### 2026-11-06: the animated triangle on the panel (QEMU)

- After `flush_resource` and `blit` were added (the swap had called a NULL pointer), `gbm_tri` ran three frames cleanly in the guest (mode set, then page flips accepted, no flip-event timeouts), and then indefinitely. With `-n 0 -s 4` running, two QEMU screendumps six seconds apart show the Gouraud triangle at different angles on the 800x480 panel: the whole path (GLSL, NIR, the mbx compiler and context, kmsro's dumb buffer imported by PRIME into the MBX's address space, the kernel driver's submit, the model's render, omapdrm's scanout) works in the guest.
- The first attempt to look had a mistake of mine: I read a `tail -40` of the run's output as a hang, when the full output ended with `gbm_tri: 3 frames`. Reading the end of the file settled it.
- Method: `scripts/panel-shot.sh` starts QEMU with a monitor socket (`run-qemu.sh`'s new `MONITOR`), runs the command in the guest through `scripts/guest.py`, and sends `screendump` to the monitor; `scripts/ppm2png.py` made `docs/gbm_tri-panel-a.png` and `-b.png`.
- Sources: the screendumps themselves (`docs/`); Mesa 26.1.8 `src/gallium/drivers/lima/lima_resource.c` and `vc4_context.c` for the hooks a render-only GPU must set; the run outputs `build/m4.out`, `build/m5.out`, `build/m6.out` (not committed).

### 2026-11-06: the parameter buffer and its free list in the driver

- The TA bins triangles into pages it takes off a free list; the model (`mbx/pb.c`, `mbx/core.c`) puts page `p` at device address `p << 12` and keeps the list as a table of 16-bit entries in device memory (entry 0: the page last handed out; `0x828` the first page, `0x82c` the last, withheld until a close; `0x838` builds it and raises `EVM_DALLOC`). So the buffer must sit at fixed device addresses in every address space.
- `mbx/drm`: `mbx_pb.c` creates one shmem object (the table, then 1024 pages: 4 MB) at probe; `mbx_mmu.c` maps it in each file's space at the top (pages 7168 to 8191, the 16 KB table just below at `0x1bfc000`) and limits the buffer allocator to the range below; `mbx_job.c` builds the list once after each reset, through the job's space, and waits for `EVM_DALLOC`. Jobs run one at a time and a render gives its pages back, so one buffer serves every file. The host shim (`mbx_sim.c`) does the same.
- Not done: an overflow (`TA_OFLOW`) is still an error; a partial render (close, render with depth stored, resume) is next on the kernel side.
- Sources: `mbx/pb.c` (`page_alloc`, `fl_entry`, `pb_render`, the header comment), `mbx/core.c` (`fl_build`, the register writes), `mbx/README.md` assumptions 22 and 26, `mbx/emu.h` (the register names); the kernel API: `kernel/build/linux-6.18.58` `include/drm/drm_gem_shmem_helper.h`, `drivers/gpu/drm/drm_gem_shmem_helper.c` (`drm_gem_shmem_create`, `drm_gem_shmem_get_pages_sgt`), as for the other buffers; the guest evidence: the emulator's register trace (`MBX_TRACE=1` through `scripts/panel-shot.sh`).


### 2026-11-06: asynchronous submit with fences

- `SUBMIT` no longer waits: it copies the stream and the buffer list, takes the file's address space by reference, collects the `dma_resv` fences of the listed buffers as dependencies (before adding its own), adds a job fence to each buffer (write or read usage by the flag), numbers the fence and queues the job on an ordered workqueue (`job_wq`). The worker waits for the dependencies, runs the TA/render sequence under the job lock, sets status and signals the fence. `WAIT_FENCE` (uapi ABI 3) waits for a number; buffer unmap flushes the jobs first.
- Mesa: a `pipe_fence_handle` holds the fence number; the colour resource remembers its last fence; `flush` returns a real fence unless done; clears, `transfer_map` and `fence_finish` wait on it. The host shim hands out numbers and treats the wait as a no-op (its submit still runs inline).
- Checked: host sim picture byte-identical at four angles; in the guest the triangle still animates on the panel (149 TA starts, the free list built once).
- Open: with the async module every guest run (four of four) prints `omapdrm omapdrm.0: atomic complete timeout (pipe 0)!` at the first mode set; the two runs before the change (`shots-pb`, `shots-trace`) printed only the RFBI `FRAMEDONE not received` line. Machine load was 6 to 8 during all of them and the guest booted slower (crng at 12 s against 7 s), so load is not ruled out, and the blocking module was not rebuilt for a same-conditions comparison. Decision with the user: leave it for the real N810, where the display timing is real.
- Sources: `mbx/drm/mbx_job.c`, `mbx_drm.h`; kernel API: `kernel/build/linux-6.18.58` `include/linux/dma-fence.h`, `include/linux/dma-resv.h`, `drivers/dma-buf/dma-fence.c`; the guest logs `build/shots-async`, `shots-rep1..3`, `shots-pb`, `shots-trace` (git-ignored); the load figures: `uptime`/`/proc/loadavg` during the runs; user (leave the timeout for real hardware).

### 2026-11-06: blending from pipe state

- `mbx_draw.c` (`hw_blend_bits`) puts the bound blend state into the ISP/TSP state word the model reads: bit 13, the source factor code in bits 29-31, the destination's in 26-28, bit 5 for SRC_ALPHA_SATURATE (a source factor of zero code plus the bit), bit 8 (no colour written) for a zero colour mask. The chip's codes name "the other side's colour", so SRC_COLOR as a source factor and DST_COLOR as a destination factor have no encoding; neither do separate RGB/alpha factors, other equations, constant-colour factors or partial colour masks. These log a warning once and draw unblended; they belong with the CPU fallback.
- Checked on the host sim only: `mesa/scripts/test-blend.sh` draws the triangle over the grey clear with (ONE, ONE) and (DST_COLOR, ZERO) and compares all 1144 triangle pixels with the plain picture: min(255, s + 32) and (s * 32) >> 8 (255 counted as 256), 0 mismatches; the four-angle comparison with the emulator is unchanged. Not checked: the chip itself, where assumption 24 (fixed-point blending) was observed but these state-word bits were taken from the model.
- Sources: `mbx/render.c` (`factor8`, the blend branch of the pixel loop, the `ST_*` bits), `mbx/README.md` assumption 24; Gallium's blend factor enum: `mesa/src/gallium/include/pipe/p_defines.h` (Mesa 26.1.8 tree under `build/mesa/mesa-26.1.8`).

### 2026-11-06: the device rootfs and the flash flow (M5 preparation)

- Read, at the user's request, `~/n8x0-kernel/N8X0_NOTES.md` and the overlay in `~/n8x0-kernel/buildroot/overlay/` (the files the stock Maemo initfs expects of the rootfs: `inittab`, `S02clock`, `S02uartpm`, `S40usbnet`, `udhcpd.usb0.conf`, `telinit`, the udhcpc NTP hook). Copied to `rootfs/board/n810-device/overlay/` with the device's own `/etc/fstab` (read over ssh: it leaves `/tmp` alone, the initfs bind-mounts its tmpfs there). The live unit (read-only look through the jig host): Linux 6.18.58 #143, the kexec loader in the kernel partition (`/boot/zImage`, then `/boot/zImage.old`), the full Maemo-side rootfs (wifi, Bluetooth, GPS, Imagination's `mbxaccess` and `S60mbxd`), mtd layout as in the notes (rootfs 0xf980000).
- `DEVICE=1 rootfs/scripts/build-rootfs.sh` builds `rootfs/out/rootfs-device.jffs2` from the same Buildroot tree: the device overlay, root password `n810`, busybox with `udhcpd` and `ntpd` (`board/n810-device/busybox.fragment`), and `/boot` from `scripts/stage-boot.sh`: this repo's kernels (the mbx DT node, `CONFIG_DRM_MBX=m`) as `zImage` and `zImage-n800`, and the kernel the unit ran (copied from it, `scripts/fetch-device-kernel.sh`) as `zImage.old`. The device overlay's `S35mbx` does not load the driver at boot (`touch /etc/mbx-autoload` to change that): the first probe is by hand.
- Checked in QEMU only: the device image boots there (root/n810 over serial, `g_ether` loaded by `S40usbnet`, `mbx` not loaded). Not checked: the initfs hand-over (`linuxrc`, DSME, BME), which only the real unit has.
- `scripts/device/`: `stage-on-jig.sh` (copies the image and helpers to `~/n810/mbx-test` on the jig host), `flash-mbx.sh` (the rootfs only, `0xFFFF -m rootfs:...`; the kernel partition's loader, the initfs, the bootloader and the CAL stay), `restore-state.sh` (the unit's wifi/dropbear/clock state from the Oct 28 rootfs backup).
- Sources: `~/n8x0-kernel/N8X0_NOTES.md` and `~/n8x0-kernel/buildroot/overlay/*` (user-pointed, outside this repo); `kernel/loader/init.c` (the order of `/boot/zImage`, `zImage-n800`, `zImage.old`); the unit itself over ssh (`/etc/fstab`, `/proc/mtd`, `/boot`, `lsmod`); `~/n810/flash-back/flash-back.sh` and `restore.sh` on the jig host.

### 2026-11-06: the first flash of the device rootfs: a panic loop, and why

- Flashed `rootfs-device.jffs2` through the jig host (NOLO caught 0xFFFF only after a manual power cycle: a software `reboot` from the running system gives no flasher window). The loader kernel kexec'd our new `/boot/zImage` (kernel `#2`), the stock initfs ran DSME and BME and mounted our rootfs, then `init` exited with status 2 and the kernel panicked ("Attempted to kill init! exitcode=0x00000200") on every boot. The user powered the unit off. Nothing else on the unit was touched: no kernel partition, initfs, bootloader or CAL; the pre-flash rootfs is backed up on the jig host (`~/n810/mbx-test/rootfs-before-20261106.tar`).
- Cause (read in the stock initfs's `linuxrc`, lines 497-511, which the user allowed me to read from the firmware part; extracted with `scripts/jffs2x.py`): it ends with `exec /usr/sbin/chroot . /sbin/init $def_runlevel <dev/console >dev/console 2>&1`, redirecting through `dev/console` of the NEW root before devtmpfs exists. Our Buildroot image had no device nodes in `/dev`; the stock rootfs has a static `/dev/console`. The shell died with `cannot open dev/console` (status 2). The serial log of the real unit cut the message to "/sbin/init"; the offline reproduction below shows it whole.
- Reproduced offline (`scripts/qemu-initfs.sh`): QEMU with the stock initfs in the initfs partition and our image in the rootfs partition, kernel `root=1f03`, as the unit boots. The failing image reproduces the panic and the message; the fix does not. Fix: `board/n810-device/device_table.txt` (static `/dev/console`, `null`, `tty`, `ttyS2`), added to the device defconfig by `build-rootfs.sh`. With it the unit's whole hand-over works in QEMU: `init`, the services, `n810 login:`.
- Still odd, not fatal: `mount: Mounting /tmp on /mnt/new_root/tmp failed: No such file or directory` appears in QEMU even though `/tmp` is in the image (1777); and `/etc/osso_software_version` (the old rootfs has it) is absent in ours. Whether the stock rootfs prints the first one is unknown (not checked; the user asked not to boot the Diablo rootfs). Effect: the initfs's `/tmp` (DSME and BME sockets) is not bind-mounted into our rootfs, which our own tests don't need.
- Sources: `linuxrc` of the firmware initfs part (`RX-44_5.2008.43-7/initfs`, user-permitted read, outside the repo); the serial log of the unit (`~/n810/mbx-test/serial.log` on the jig host, captured through `/dev/ttyUSB0` with sudo, as the user's `n810-net.sh` does); `build/initfs-boot.log` (QEMU runs); Buildroot's device table format (`build/buildroot-2026.08.1/system/device_table.txt`).

### 2026-11-06: first MBX on the real N810: it probes, the TA and render complete, the picture is wrong

- On the unit (kernel `#2`, our rootfs, serial capture on the jig host): `modprobe mbx` by hand gives `mbx 50000000.gpu: MBX core id 0x01010003, revision 0x5010307` and a render node (`renderD129`); no hang, no kernel message. Both values equal what the emulator models (`mbx/core.c`, taken from the user's earlier N810 probe). So the GFX power-up/reset path of the driver (PRCM clocks, soft reset) works on the chip.
- `mbxtri-drm` (the fixed 64x64 triangle stream, through SUBMIT) returns success and the job completes, but the colour buffer is not the triangle: the first run left exactly the bottom-right 2x2 tiles (4 x 256 pixels) filled with near-white noise (262 distinct colours; R/G/B 94..255) and the other 12 tiles at the CPU-filled clear colour; later runs fill all 4096 pixels with different noise each time (md5 differs run to run). `docs/n810-tri-device.png` is the first run. (The tool counts any pixel that is not the clear colour as "drawn", so its "1024 of 4096" and exit status 0 were not a pass.)
- `mbxtri-drm -D` dumps the region array, the tails buffer and the background buffer after the job. Real chip against the model (host shim, same stream): the region array has 16 entries, one per tile, word 1 a list pointer into the parameter buffer, in the same layout; on the chip word 0 has bits 31 and 26 on the last tile like the model but never bit 29 (the model's `0x20000000`, "Z loaded for this entry", `mbx/render.c`); the list pointers start at `0x01c01144` (the model: `0x01c01000`, 0x40 apart), and advance by exactly 0x400 per submit (`0x1144, 0x1544, 0x1944, 0x1d44`), which agrees with README assumption 26 (a scene goes on from where the last stopped). The tails and background buffers stay all zero in both. Reading: the TA binned the triangle into all 16 tiles; whether bit 29 is cleared by the render (the model's `ta.c` comment says a render leaves flags in the entries) or never set is not known, since the dump is taken after the render.
- Not known why the render reads garbage. Candidates, none tested: the render reads parameter-buffer data the TA wrote through a path the CPU or the MMU does not see the same way (page-table or TLB state between TA and render, a write-back or flush the chip needs after TA_COMPLETE); stale data in pages of the free list; the vertex and object data the TA writes first (the first 0x144 bytes of the page, which the model does not write there); a register the model does not need. Next probes: read the parameter-buffer pages after a job (a debugfs or ioctl dump in the driver) to compare the TA's output with the model's; run the TA only and render separately; vary the buffer's cache attributes.
- Checked here only: the three runs on the unit and the host shim comparison above. Sources: `mbx/tools/mbxtri_drm.c` (`-D`), `mbx/ta.c` (region array comment, `w0` bits), `mbx/render.c` (line ~1437, bit 29), `mbx/README.md` assumptions 5 and 26, `mbx/core.c` (`CORE_ID`, `REVISION`); the unit's serial log (`~/n810/mbx-test/serial3.log` on the jig host) and dmesg.

### 2026-11-06: the background object, what the chip does without one, and a kernel-memory hazard

- Reading `mbx/render.c`: the emulator's render draws from the TA's in-memory scene and never reads the parameter buffer's lists or objects from device memory (`pb.c` writes a copy of them, nothing reads it back), and it reads only the background object's two texture words from `0x83c`. So the model forgives anything wrong in the parameter buffer; the chip does not. That is the main gap between emulator and chip, and the next work on "equal behaviour" (below).
- New: `DRM_IOCTL_MBX_PB_DUMP` (root only; the kernel's debug window on the parameter buffer, also in the host shim) and `mbxtri-drm -P file`. What the real TA wrote for the triangle (first scene after loading the module), against the model's: the same object block of 16 words (state word `0x00400200`, three vertices x y z and a packed colour) at page 0 offset `0x158` (the model: offset 4), the vertex floats rounded (`40cc0000` where the model has `40cccccc`; `42668000` for `42666666`); tile list items `40900055, f0000000` where the model has `40f00000, f0000000` (the low field is the object's word offset from the buffer's base, `0x55` = byte `0x154`, the word before the state word); the free-list table filled as `entry[p] = p + 1`. The first `0x140` bytes of page 0 stay zero, which is where README assumption 6 puts the background object (its words 1 and 2).
- Finding (observed on the unit): the tile start reads that background object. Passing a zero page or zeros there makes the chip flush its uninitialised tile memory: near-white noise (all-ones with single bits cleared, ~260 distinct colours; the same statistics every run) over the whole target, or over only some tiles in the very first run. README assumption 6's "none set up: the colour buffer itself" holds for the model only. With the background words describing the render target itself as a linear texture, written by the kernel into the buffer's first words (`mbx_pb_set_background`, flag `MBX_SUBMIT_BG_TARGET`, `0x83c` = the buffer's base, `mbxtri-drm -K`), an **empty scene** (`-E`, just the end word) leaves all 4096 pixels as the CPU filled them, on the chip and in the model alike (0 pixels differ). Without the flag the same empty scene changed 256 pixels (one tile) on the chip.
- **Hazard (observed):** in the runs with an invalid background object the kernel reported `BUG: Bad page map in process mbxtri-drm-arm  pte:fffdffff` (and `ffffff7b`, `bfffffff`, `e2ffffff`, ...) for the process's stack pages: the page-table entries held the same noise. So the chip wrote tile data to memory outside our buffers, into page tables. Anything wrong in what the render is given can corrupt kernel memory; the driver must not hand the chip a background object, region array or lists it has not built. Next in the driver: build the background object always (r_pb ignored), validate register values, and consider where the chip may write (its pages are bare physical page addresses, not protected: README assumption 8 / the MMU isolation item in the probe list).
- Still wrong: the triangle, with the correct background, still comes out as noise (259 colours, 544 pure white), so what the ISP does with the objects is not yet right; no `BUG` appeared in that run. Open: the objects' state or texture words (the chip may need blocks the model does not), the vertex format (the chip stored rounded floats), or a register the model never checks.
- Sources: `mbx/render.c` (`render_kick`, the background read), `mbx/README.md` assumption 6, `mbx/texture.c` (`tex_decode`: texture word A and B layout); the unit's dmesg and the dumps (`build/pb-dev.bin`, `pb-sim.bin`, git-ignored); `mbx/drm/mbx_pb.c` (`mbx_pb_set_background`, `mbx_ioctl_pb_dump`).

### 2026-11-06: sweeping the triangle's state on the chip: nothing changes the noise

- `mbxtri-drm` takes `-C` (render control `0x610`), `-M` (sampling `0x61c`), `-S` (the object's state word) and sets the clear depth `0x6dc` as GLES does. On the chip, with the background object right (`-K`) and no kernel `BUG`: state `0x00400200` (the default), `0x00400e80` (depth always, no write), `0x00000e80` (flat), `0x610` = 0, `0x10000`, `0x20000`, `0x61c` = 0, `0x20000`: every variant gives noise of the same kind (259 distinct colours, 538-544 pure white pixels, no pixel of the clear grey left, pixel values different each run). In the model the same stream draws the triangle (1144 pixels). Tiles with no objects (an empty scene) are left untouched on both. So the chip's output for any tile that has an object does not depend on these registers or on the state word.
- Sources: the unit (`mbxtri-drm -K` runs, `build/sweep/`, git-ignored).

### 2026-11-06: the emulator learns what the chip needs (the contract)

- Background: on the unit nothing the driver did for a scene was enough (section 6, the entries before this one), and the emulator had hidden it: its render draws from the TA's in-memory scene, the TA writes the whole region array itself, and nothing it reads from the parameter buffer, the tails or the contexts is checked. What a client must do (the free list's registers and first page, the contexts, a whole 26-word background object, an initialised region array with bit 30 before the render, 64 KB aligned zeroed tails) was supplied by the user as found during device testing; none of it is a capture of ours.
- Done in the model: README assumption 31; `emu.h` `V_*` and `emu_violation()`; `ta.c` `check_contract()` at TA start; `render.c` checks the background object (whole or not) and bit 30 at the kick; strict mode (`mbxemu_set_strict`, the default) gives white in the tiles that hold objects when anything fails, and white as every tile's start for a bad background object (the old "the target's own pixels" fallback is only in forgiving mode); every miss is counted in `mbxemu_stats` and logged once a scene as `contract: ...`. `lib/mbx_contract.h` does every step for a client (the host tools, the tests and the Mesa shim use it; the kernel driver mirrors the formulas). `tests/t_contract.c` leaves out each requirement in turn (eight cases, strict and forgiving): counted, garbage in strict, the triangle in forgiving. `t_pb` and `mbxtri` now follow the contract (`mbxtri` runs with the MMU on: the background object's texture word cannot hold a physical SDRAM address) and draw the same picture as before.
- Interface (ABI 4): `BO_INFO` takes `MBX_BO_INFO_ALIGN_64K` (tail pointers), `SUBMIT` ignores `r_pb` and the driver builds the background object, initialises the region array, zeroes the tails and sets bit 30 (the `MBX_SUBMIT_BG_TARGET` experiment is gone). Mesa allocates the tails as 64 KB aligned and no longer a background buffer. The Mesa shim does the contract's steps; through it the GLES2 triangle is byte-identical to the emulator-only picture at four angles and the blend test passes, with no violation logged.
- Not done: the kernel driver does not do the contract yet (the real unit and the QEMU guest both need it), and the model's render still does not read the parameter buffer's lists and objects from memory.
- Sources: user (found during device testing); the unit's runs described in the entries before this one; `mbx/render.c`, `mbx/ta.c`, `mbx/core.c`, `mbx/pb.c`, `mbx/README.md` assumptions 6 and 31.

### 2026-11-06: the kernel driver does the contract; correction: the render skips empty tiles; still noise

- Kernel (`mbx/drm`, ABI 4): the free list's registers and first page (base + 1), `0x83c`, `0x6d8`, `0xce0`; the TA and EVM contexts (a zeroed 32 KB object mapped in every space below the free list's table, loaded with `0x820`, `0x864`, `0x810`, `0x818` once per reset); the 26-word background object written into the buffer's page 0 from the render target (`mbx_pb_set_background`); the region array initialised and the tails zeroed through the client's own buffers (`mbx_vm_cpu`); bit 30 on every entry before the render; `SUBMIT` checks the tails are 64 KB aligned and 64 KB and the region buffer is big enough; `BO_INFO` maps with 64 KB alignment on request. Debug parameters: `tiles_mode` (how `0x860` is programmed) and `bg_shift` (bytes added to the background texture's address); `DRM_IOCTL_MBX_PB_DUMP`. Mesa's tails buffer is 64 KB aligned; the Mesa shim does the same steps and the GLES2 triangle through it is byte-identical to the emulator-only picture at four angles.
- On the unit with all of it (no kernel `BUG` any more, no corruption seen): the empty scene `-E` leaves every tile but the last untouched. The parameter buffer's page 0 holds exactly the 26 words written; the TA's object block sits at byte `0x484` and the list items point at it (`0x40800121` = word `0x121`), consistent; the region entries after the render read `0x40000000 | tile`, bits 29 and 25 cleared by the chip. The triangle is still noise (259 colours, ~540 pure white, different each run).
- **Correction** of the entry "the background object ...": its claim that a whole background object fixes the empty scene was wrong. Tests with a target filled with one distinct value per pixel (`-G`) and the background texture shifted by 128 and 4096 bytes (`bg_shift`): the output equals the CPU-filled target in every tile in both cases, never the shifted pattern. So the chip does not draw the background in tiles without a list (README assumption 5 says the model does); an empty scene looked untouched because its tiles are skipped, not because the background was right. The one tile that does change is always the last, the entry with the end mark: with the full background object and no shift it comes out as noise (151 distinct values, mostly ones); with the texture shifted it is unchanged. Why the shift matters there is not known.
- Tried and found to change nothing: the encoding of `0x860` (seven encodings of the tile count: none, tx*ty, the padded count, minus one, the packed grid, ...); the state word, `0x610` and `0x61c` (earlier sweep); the clear depth.
- Open, in the order I would look: (1) what a tile with an object does with the background object (the chip may draw it as an object, with the texture read through a path the MMU setup does not cover, or from a buffer it is also writing); (2) whether the first job after loading the module differs from later ones (the first triangle run left only the last 2x2 tiles as noise, later runs all of them); (3) registers the stock stack writes that the model does not know (a capture of one frame's register writes from the stock stack is the thing to get); (4) whether noise tiles are a texture fetch of unmapped memory (the dummy page) or of the colour buffer itself.
- Sources: the unit's runs (`build/out2.bin`, `build/g0.ppm`, `build/sweep2/`, git-ignored); `mbx/drm/*`; `mbx/README.md` assumptions 5 and 31; user (found during device testing) for the contract.

### 2026-11-06: more of the contract (found during device testing); the noise is not constant

- Added to the model, the kernel driver, the shim, Mesa and the tools, from the user's description of the stock stack's flow (README assumptions 31 and 33): the parameter buffer filled with `0xf0000000` end words before the free list build; `0x854` and `0x858` = `0x3727c5ac` before every TA start; `0x85c`'s base bits `0xe1`; the stream starting with the region clip control (`0x10000010`, 0) and a depth-only clear to the far plane (a screen-space object through a one-instruction passthrough in section bit 11, kept in the last VGP slot); in the model, tile depth starting as NaN when a scene has no such clear (`V_DEPTH_CLEAR`). The model also counts, softly (not confirmed on the device, never garbling the picture): alignments, the tile grid, the render's format against the background object, the free list's chain, `0xe1`, the render register set, the parameter buffer's fill, and writes outside the known buffers (a write watch); and, hard: a region entry with neither a list nor bit 25. `tests/t_contract.c` has 42 checks. The Mesa shim stays byte-identical to the emulator at four angles with the clear in Mesa's stream.
- Not done: the render registers whose values are not known (`0x60c 0x614 0x618 0x644 0x648 0x64c 0x6d0`, `0x860`'s encoding) are still not written, and the model counts the omission every frame; two region arrays; the model's render still draws from its own scene.
- On the unit with all of it: still noise, but not the same every time. After a fresh module load the first job sometimes leaves all 4096 pixels untouched (nothing drawn) and sometimes comes out as noise in all 16 tiles; later jobs are noise, with an occasional untouched one (job 6 of 8 once). Two series of identical jobs (back to back, and 3 s apart) both began with an untouched job 1; runs with the diagnostic options (`-R`, `-P`) or the trace on began with noise. No kernel `BUG`. So the result is not a fixed function of what the driver sets up: a race or state left over (cache, the free list or contexts across jobs) is likely. The trace in `docs/n810-trace.txt` is of a noisy first job.
- Sources: user (found during device testing); the unit's runs (`build/sweep*`, `build/trace/`, git-ignored); `mbx/lib/mbx_contract.h`, `mbx/README.md` assumptions 31-33.

### 2026-11-06: the render registers with their values; two region arrays; the pattern is regular after all

- The user gave the values of the render registers the model did not know (found during device testing): `0x60c` = 0, `0x614` = `0x618` = `0x1e3ce508`, `0x644` = 0, `0x648` = 1, `0x64c` = 0, `0x6d0` = 1, and `0x860` = (tiles_y - 1) << 16 | (tiles_x - 1) (`0x00030003` for 4 x 4 tiles; the macro tile's width in bits 4-6 of X, 0 up to 32 tiles; only known up to 16 tiles a side). The driver, the shim, Mesa's tools and the tests write them (`mbx_contract_render_regs`); the model checks `0x860` against `0x85c` (soft) and, in strict mode, loses the pages a render would give back when they disagree (the chip frees the wrong ones and the free list leaks until the TA hangs). Mesa uses two region arrays by turns (`tbos.regions[2]`, `which`), the render's `0x608` the one the TA used. `t_contract`: 43 checks.
- On the unit with the whole set: the same result, now regular. After a fresh module load the first job leaves every pixel untouched, and every later job is noise in all 16 tiles; three fresh loads of six jobs each gave exactly that. The earlier "not constant" runs were the diagnostics: dumping 16 KB of registers before the first job (`-R`) delays it, and then the first job is noise as well. So a render right after the load does nothing and later ones render, to noise; the chip may not be ready right after the load. Nothing in what we set up changed the noise (the contract, the register values, the clear).
- Sources: user (found during device testing); the unit's runs; `docs/n810-trace.txt` regenerated with the complete register set (write order: the unknown render registers, `0x860`, then `0x608`...`0x6dc`, `0x6ec`, `0x680`).

### 2026-11-06: correction: the "first job clean" and the varying results were my tool racing the job

- `mbxtri-drm` read the picture right after `SUBMIT`, which since the driver went asynchronous only queues the job: on the unit it raced the render. That explains every "progress" result since then (0, 4, 256, 1024 or 4096 changed pixels; "the first job after a load leaves everything untouched"; results depending on the diagnostics' delay) and the entries above that read them as chip behaviour ("the chip may not be ready right after the load", "not a fixed function of what the driver sets up"). The tool now waits for the job's fence (`DRM_IOCTL_MBX_WAIT_FENCE`) before reading. With the wait, every job on the unit, the first included, is noise in all 16 tiles (259 distinct colours, ~540 pure white): regular and independent of everything we have changed. The host shim runs a job inside `SUBMIT`, which is why only the unit showed it. Earlier sweeps (`0x860`, state word, `0x610`, `0x61c`, clear depth, `bg_shift`) all gave full-picture noise either way; the `bg_shift` conclusion about empty tiles used jobs that completed before the read and still stands only as far as that.
- Done after the user's second look at the trace: the driver now stores the context as the stock daemon does (clear TA_CONTEXT, `0x818` = 1, poll for it up to 50 ms, clear again); a debug parameter `ta_extra` ORs bits into `0x85c` (the stock stack sets bit 18, `0x40000`, with no external Z): with it, the same noise. A readback of `0x860`, `0x614`, `0x618`, `0x854`, `0x85c` and `0x610` before the kick is in the trace (`trace=1`). The trace generator printed the free list table's entries wrongly (the low half of each 32-bit word: entries 2, 4, 6 as "3, 5, 7"); fixed, the table is the chain it should be.
- Sources: the unit's runs with the fence wait; `mbx/tools/mbxtri_drm.c`; the user's reading of `docs/n810-trace.txt`.

### 2026-11-06: what the noise is: a tile buffer nothing drew into

- With the fence wait (reads reliable), streams on the unit: no objects (`-E`): only the last region entry's tile (256 px) is processed and comes out noise; the depth-only clear alone (`-X 1`): all 16 tiles noise; the triangle alone (`-X 2`) or both: all tiles noise. So any object in a tile gives noise, even one that writes no colour; the per-tile pass is what fails, not the objects.
- The background object's texture address: equal to the target's address: the last tile (empty scene) comes out noise; the address moved 128 B, 4 KB or 16 KB: nothing in the target changes at all (neither the shifted pattern nor noise). A flipped background winding (`bg_mode` 1) changes nothing, so culling is not the reason.
- The noise itself: with the target holding a unique value per pixel, the noisy tile's pixels have no relation to what was there, and the number of cleared bits in each 24-bit pixel (of all ones) follows a Poisson distribution (mean about 1.9: each bit cleared independently with probability about 8%; counts 0..7 over 256 pixels: 32, 77, 78, 39, 15, 8, 6, 1). That is not a colour calculation: it reads like a tile buffer the render never wrote, flushed as it was (ones with bit errors). Reading: the background pass and the objects do not write pixels on this chip as we drive it; when the background texture is the target the flush happens, when it is not nothing is flushed.
- Tried without effect: `ta_extra` = `0x40000` (the stock stack's `0x85c` bit 18), the daemon-style TA_CONTEXT store, the flipped background winding, every earlier encoding of `0x860`, the state word, `0x610`, `0x61c`, the clear depth. Not yet tried: whatever enables or starts the pixel pipeline that we may not write (clocks or enable bits beyond the three PRCM clocks; a texture-cache or TLB flush before the render; `0x1020`'s other bits); another stream.
- Sources: the unit's runs (`build/shift/`, `build/flip/`, git-ignored); `mbx/tools/mbxtri_drm.c` (`-X`, `-E`, `-G`), the driver's `bg_shift` and `bg_mode` parameters.

### 2026-11-06: the MMU protocol made no difference; the "noise" is two nearly fixed tile patterns

- From the user's description of the stock daemon's MMU handling (found during device testing): the enable handshake on `0x1020` bit 16, TLB loads for every mapped page (`0x1024`, `0x1028`, bit 8, bit 24), sixteen soft resets with interrupts off. The driver has switches for each (`mmu_seq`, `tlb_mode` 1 and 2, `reset16`); seven combinations on the unit (single, pairs, all): the output is unchanged. The model counts the first two as soft contract violations (`V_MMU_SEQ`, `V_TLB`); `t_contract` has 45 checks, including a variant that follows the protocol and trips neither.
- Analysis of the pictures (reads after the fence): across ten runs of different settings, 4048 of 4096 pixels have the same value every time; about 40 differ from run to run. The 16 tiles of one run repeat one of two patterns (253 of 256 pixels equal between tiles of the same parity; period 2 in tile columns, one tile row in rows). A run with an empty scene and a gradient-filled target produced a different pattern in the one tile it processed (6 of 256 pixels equal to the full scene's). So the output does not depend on what the target held, nor on the registers and settings tried, only slightly on the stream. The picture is the chip's two tile buffers being flushed with nothing drawn into them, holding a stale or fixed pattern (`docs/n810-tile-flush.txt` has the 16 x 16 patterns). Open: why nothing is drawn into the tile buffers, and what the pattern is (the stale content of an earlier use, a fixed fetch, memory behind an unmapped read).
- Sources: user (found during device testing) for the MMU protocol; the unit's runs (`build/mmu/`, `build/shift/`); `docs/n810-tile-flush.txt`.

### 2026-11-06: the stock kernel sources, the render timing, a wrong idea about the list pointers, and the parameter buffer moved low

- `mbx/kmod` (the Imagination access driver, GPL): its GFX power-up is what ours was ported from (same clocks, 2D/3D at L3 / 2, `VALID_CONFIG`, `PM_PWSTCTRL_GFX`, the reset pulse), so nothing differs there. `hwdefs/mbx1defs.h` names the interrupt bits (0 `CMDPROC`, 3 `ISP`, 7 `TA_TIMEOUT`, 9 `TA_STREAM_ERR`, 10 `2DSYNC`; `INT_ALL` `0xffff`, `INTE_ALL` `0x87ff`); the user says bit 3 is a software doorbell the clients write to `0x12c`, yet on the unit it is set at the end of each render without our writing it. Its interrupt handler masks everything and hands the status to user space. Device memory in the stock services is single pages from `alloc_pages(GFP_KERNEL, 0)` (not contiguous) mapped uncached or write-combined, like ours. The device arenas in `sysconfig.h` are 0-8 MB and 8-32 MB.
- Timing with the trace: the TA takes 2.6 ms for the 64 x 64 stream (a steady figure); kick to RENDER varied from 0.4 to 6.2 ms for the same job (396, 6195, 3479 us); the user's figure for a normal frame is 7-10 ms. The alpha byte of the flushed picture shows the same independent bit clearing as the colour bytes (about 8% of all 32 bits).
- A wrong idea, tried and dropped: that the lists' pointers are 20-bit word addresses, so the parameter buffer at 28 MB made the ISP fetch from the wrong place (`0x40800121` for an object at `0x1c00484`). I moved the buffer, its table and the contexts to the low 4 MB (buffer at 1 MB: `0x100000`, 768 pages, table at `0xfc000`, contexts at `0xf4000`, clients' buffers above 4 MB: as the user says the stock stack has it at device 0), and the same noise came out. A dump of the new layout shows why the idea was wrong: the object at `0x101400` has the list item `0x40800500`, a word offset of `0x500` from the buffer's base (`0x83c`), not a truncated absolute address (that would read `0x40500`). The region entries' list addresses are absolute (`0x00101000`). The low layout stays (it is what the stock stack uses) but the model's `V_PB_HIGH` check I had added is removed.
- Sources: `mbx/kmod/gpl/driver/mbxaccess.c`, `mbxaccess_iowrapper.c`, `hwdefs/mbx1defs.h`, `system/omap24xx/sysconfig.h`, `services/env/linux/mm.c`; the unit's runs; user (found during device testing).

### 2026-11-06: the parameter buffer at device address 0: the first triangle on the real chip

- The user's note on the stock layout (the buffer at device 0, the free list's table and contexts at `0x800000` in the second arena; the lists' pointers relative to `0x83c`, the region entries' addresses absolute, so they agree only at base 0; whether the ISP applies the base to the links was not proven either way) suggested the cheapest test: put our buffer at 0. The driver now has the pages at device 0 (768 pages), the table at `0x800000`, the contexts at `0x804000`, the clients' buffers from `0x810000` (separate objects for the pages, the table and the contexts; `mbx_drv.h`).
- On the unit: the noise is gone. A job leaves 3042 pixels at the clear colour and draws 1054 (every job, three in a row, no kernel `BUG`). The model draws 1144 pixels for the same stream; the device's pixels are a subset (nothing outside the model's triangle), same bounding box (x 7-56, y 7-50). The colours are not the model's: 973 of the 1144 triangle pixels equal the expected colour ANDed with `0x20` (the clear colour) per channel; 171 are mixed (some channels exact, some ANDed), mostly at the right edge of the triangle's tiles and the lower right; 11 pixels near the apex are exact. A bitwise AND with the old pixel looks like a logic op or a write that combines with the old colour, not an arithmetic blend. Not yet looked at: the registers `0x644 0x648 0x64c 0x6d0` (written as the stock stack does, their meaning unknown), the state word of the triangle (`0x00400200`), the colour mask.
- Host side: `mbxtri`, the shim, `t_pb` and `t_contract` use the same layout (PB at 0; the small buffers above `0x810000`); the model counts a base other than 0 as `V_PB_BASE` (hard); `t_contract` 45 checks; the Mesa path through the shim stays byte-identical to the emulator at four angles.
- Why earlier results looked like noise: with a non-zero base the ISP evidently read nothing usable, whatever the pointers meant; the flushed picture was the stale tile buffers. The sections above that chase the MMU, `0x860`, the clear and other settings were looking in the wrong place; their findings (the register values, the contract) are still right but did not matter for the noise.
- Sources: user (found during device testing) for the layout; the unit's runs (`build/z*.ppm`); `mbx/drm/mbx_drv.h`.

### 2026-11-06: the triangle's colour is ANDed with what the target held

- With the parameter buffer at 0 the picture is the model's shape; the colours are the model's colours ANDed with the old pixel. How it was shown on the unit (reads after the fence): the target filled with ones gives exact colours (913-940 of 1144 triangle pixels), with `0x20` grey `colour & 0x20`, with black black, with `0x80` `colour & 0x80`; pixels outside the triangle keep the fill. The old pixel is not the background pass's output: the pass maps 1:1 (the shifted-texture gradient is right in the left half; in the right half a 128 B shift acts as 256 B, which does not matter unshifted) and a separate copy of the target as the texture gives the same AND.
- Ruled out as the cause: the state word's source and destination factor fields (all 64 codes, blend flag clear or set: one result each), bit 21, the source factor ONE (the stock stack's word `0x22600200` makes the triangle vanish through bit 25, probably a texture-stage enable); every bit of `0x648`, `0x64c`, `0x6d0` (no change); the clear object (needed: without it nothing is drawn). Bits 0 and 1 of `0x644`, bits 12-25 of `0x60c` and the render format `0x650` (values other than `0xe` and `0xd`) change coverage or give garbage, never the AND. The model has no logic op in 3D (`twod.c` has ROPs for the 2D core only).
- The state of the picture: 973-1000 pixels fully explained by the AND; the rest are pixels whose old value was zero or stale tile data (lower right, edges of tiles), which suggests the AND is with the target in memory at write-back but not everywhere (the tile's own content where the background pass did not cover?). `docs/n810-trace.txt` is regenerated with this layout and ends with a pixel-level comparison against the emulator.
- State words in pairs (triangle x clear, 28 runs, `-S` and `-K`): triangle words with bit 25 (`0x22400200`, `0x22600200`) draw nothing whatever the clear word is; without it the footprint stays 1054 px and the colour stays the target ANDed (1000 of 1144 pixels; 981 with the base bits on the clear). A clear that writes colour (bit 8 clear) paints the whole target. No state word restores the model's colours, so the emulator gets no strict state-word check yet.

- Sources: the unit's runs (`build/fill_*.ppm`, `build/regsweep.out`, `build/grid.out`, `build/sep*.ppm`, git-ignored); `mbx/tools/mbxtri_drm.c` (`-F`, `-Z`, `-X`, `-S`), the driver's `r*` and `bg_shift` parameters; user (found during device testing) for the stock stack's state word.
