# Journey: a modern GPU stack for the N810's PowerVR MBX

A running log of the work to give the MBX a from-scratch DRM kernel driver
and a Mesa Gallium GLES2 driver, aiming at an animated triangle on omapdrm,
first in QEMU and then on a real N810. Newest entries at the bottom. Facts
marked *unverified* have not been checked on the emulator or the device.
For the setup snapshot see `NOTES.md`.

**Sources.** Each entry ends with where its facts came from, so they can be
checked: repo paths (relative to the repo root), Mesa and kernel paths (in the
trees under `build/`, versions named), and URLs. "User" means a decision or
instruction the user gave in the session. A fact with no source line is mine and
unverified. Anything the emulator model says about the chip is the model's,
not the chip's (`mbx/README.md` marks which items were observed on a device).

## Decisions

- **Kernel driver:** a new DRM render node (`mbx/drm/`, GPL-2.0), written
  from scratch. The Imagination DDK shim (`mbx/kmod/`, `mbxaccess`) is not
  needed: no Nokia userland compatibility is wanted. It stays as a hardware
  reference until the new driver loads.
- **Userspace:** a Gallium driver in Mesa (`src/gallium/drivers/mbx`, MIT)
  with a DRM winsys and kmsro for omapdrm scanout, plus a `sim` winsys that
  links `libmbxemu.a` in-process so most work can be tested on the host.
- **Display:** omapdrm with fbdev emulation.
- **Hardware shape:** the VGP does vertex work (vec4, no branches); the pixel
  side is fixed-function (two texture units with a word-C combiner, offset
  colour, fog, blending; no stencil in the emulator). Vertex shaders compile
  NIR to VGP; fragment shaders are matched onto the fixed pipe.
- **Unsupported fragment shaders fall back to the CPU.** v1: the whole draw
  in software (Mesa `draw` plus a softpipe-style rasteriser on the mapped
  buffers). v2: hybrid, the VGP transforms and streams out, the CPU shades
  (needs the VGP stream-out verified on the device).
- **Submit path:** the TA stream goes to the TA port first by CPU PIO, then by
  omap-dma (fixed destination, 32-bit); DMA into the MBX is possible. The
  port stalls the bus while its 59-word FIFO is full, so unpaced DMA works
  in the model. *Unverified on the device.*
- **Mesa:** fetch it; use the version Buildroot 2026.08.1 ships.
- **No golden captures** of the real stack's TA streams exist, so the
  emulator is the only reference.

## Log

### 2026-11-06: plan and M0

- Read `NOTES.md`, the emulator (`vgp.c`, `ta.c`, `emu.h`, `mbxemu.h`) and the
  old kmod. The old driver marshals Imagination "services" calls for a closed
  userland and has no GEM, scheduler, fences or DT node.
- Plan agreed: M0 test loop; M1 kernel driver (M1b: DMA submit); M2 a no-Mesa
  triangle (`mbxtri`) on the host through libmbxemu, then in the guest; M3
  Gallium driver with the CPU fallback; M4 animated triangle in the guest via
  GBM and omapdrm; M5 the real N810.
- Found: the 6.18 tree has no MBX DT node (only `gfx_clkdm`); `DRM_OMAP=y`;
  `zImage-6.18` is 2,081,046 bytes against the 2 MB NOLO limit, so the new
  driver and `drm_gem_shmem_helper` must be modules.
- Wrote `scripts/run-qemu.sh` (flash image plus QEMU, from the NOTES recipe).
- Kernel patches: `0063` hooks `drivers/gpu/drm/mbx` into the DRM Kconfig and
  Makefile; `0064` adds `gpu@50000000` to `omap2420.dtsi` (INTC 21, clocks
  `gfx_ick`, `gfx_2d_fck`, `gfx_3d_fck`). `prepare-6.18.sh` copies `mbx/drm/`
  into the tree. `CONFIG_DRM_MBX=m` sticks and pulls in
  `CONFIG_DRM_GEM_SHMEM_HELPER=m`.
- Driver skeleton: platform probe, the GFX power-up and reset sequence taken
  from the old driver (clocks on, L3/2 rates, power domain on, reset pulse;
  order matters or the interconnect aborts every later access), the IRQ, and
  a render node with a `GET_PARAM` ioctl. It compiled and linked into
  `mbx.ko`.
- Lesson: `build-6.18.sh` takes over two minutes. A foreground timeout killed
  make mid-write and left zero-size objects and missing `.o.d` files. Fix:
  delete them and rebuild; run the build in the background.
- Rebuilt: `zImage-6.18` is 2,082,918 bytes, 14,234 under the 2 MB NOLO limit;
  `mbx.ko` and `drm_shmem_helper.ko` are in the modules tree. The rootfs
  overlay now loads `mbx` (S35mbx) and no longer stages `mbxaccess.ko`, which
  would fight the new driver over the same MMIO.
- **First light in QEMU:** the guest boots, `mbx` loads and probes, the log
  has `mbx 50000000.gpu: MBX core id 0x01010003, revision 0x5010307` and
  `[drm] Initialized mbx 1.0.0 ... on minor 1`, and `/dev/dri/renderD129`
  exists next to omapdrm's `card0`/`renderD128`. The revision register
  (0xf10) is not zero in the model; NOTES.md had only noted the product-ID
  registers at 0xfe0-0xffc reading 0.
- Next: M2, the no-Mesa triangle (`mbxtri`), host first through libmbxemu.
- Sources: `NOTES.md` (layout, what is built, boot recipe, open items);
  `mbx/README.md` and `mbx/{emu.h,mbxemu.h,vgp.c,ta.c}` (the model, TA stream
  and VGP, the FIFO that stalls the bus); `mbx/kmod/gpl/driver/mbxaccess.c`
  (the old driver's ioctls, and the GFX power-up and reset sequence in
  `MBXAccess_GfxClocks`, about lines 820-900); kernel tree
  `kernel/build/linux-6.18.58`: `arch/arm/boot/dts/ti/omap/omap2.dtsi` (the `ocp`
  bus), `omap2420.dtsi`, `omap24xx-clocks.dtsi` (labels `gfx_ick`, `gfx_2d_fck`,
  `gfx_3d_fck`), `omap2420-clocks.dtsi` (`gfx_clkdm`), `drivers/gpu/drm/{Kconfig,Makefile}`,
  `include/drm/{drm_gem_shmem_helper.h,drm_drv.h}`, `drivers/gpu/drm/drm_gem_shmem_helper.c`
  (`drm_gem_shmem_get_pages_sgt`), and the generated `build/obj-6.18.58/.config`;
  `kernel/scripts/{build-6.18.sh,prepare-6.18.sh}` (the 2 MB partition check, the
  patch flow). Decisions (new driver from scratch, no legacy userland, omapdrm,
  CPU fallback, DMA possible, fetch Mesa): the user, in the session.

### 2026-11-06: M2 step 1, `mbx/tools/mbxtri.c`

Work is confined to this repo from here on (the user asked); everything below
comes from `mbx/*.c` and its README, nothing from outside the tree.

- `mbxtri` (`make -C mbx mbxtri`) drives `libmbxemu` on the host the way a
  driver drives a frame, and renders a Gouraud triangle: 1 scene, 1 prim, 16
  tiles, 0 stream errors, 1144 of 4096 pixels drawn, correct winding, vertex
  colours red/green/blue where expected. `-a DEG` rotates it through the VGP
  constants (the image changes).
- The sequence that works, from the emulator source:
  1. TA setup: `0x85c` config = `0x80 | (tiles_x-1) << 20 | (tiles_y-1) << 10`
     (bit 7 makes the end word end the scene), `0x84c`/`0x850` size << 16,
     `0x844` region array, `0x840` tails, `0x810` context 0, `0x800` start.
  2. Stream: object header `0x10000000 | mask` with blocks in bit order
     (state, alpha test, layout, viewport x-scale/x-off/y-off/y-scale, depth
     scale/offset/1e-5); a VGP section `0x80000000 | bit`, a flags/range
     word, a code header `0x90000000 | first << 16 | last` and two words an
     instruction, constants `0xa0000000 | first << 16 | last` and four floats
     each; then the primitive word (`0x70000000` triangle list), the sections
     word (`0xf0020000 | mask`; the chip needs bit 17, observed on a real N810 by the user's probe), vertices as the layout names them, TA control
     port 1 (object ends), and `0xf0000000` (scene ends).
  3. Render: `0x608` regions, `0x610` = `0x30000` (depth on chip), `0x61c`,
     `0x650` format (0xe ARGB8888), `0x654`/`0x658` (size-1) << 16, `0x65c`
     address, `0x660` stride in words, `0x83c` background object (zero:
     the target's own pixels), `0x6ec` context, `0x680` kick.
- VGP facts used: `o1` is BGRA (a `.zyxw` swizzle brings packed RGBA right),
  inputs missing components default to 0 with w = 1, DP4 writes a scalar to
  the lanes of its write mask, so a 4x4 transform is four DP4s with masks
  X, Y, Z, W against the matrix rows in c0-c3. The TA does the divide and
  viewport: o0 stays clip space.
- Not exercised yet: the free list and parameter buffer (the model's writer
  is off without one; the real chip needs it), `0x85c`'s other bits (the
  model only reads the grid and bit 7), the ISP/TSP state bits the real chip
  may need set (the model needs none for Gouraud + depth), the MMU.
- Next: the kernel side (GEM objects, the MBX address space and page tables,
  an interrupt-driven submit with a fence) so the same stream can go through
  `/dev/dri/renderD129` in the guest.
- Sources: all from this repo's model. Register names and meanings:
  `mbx/emu.h` (`R_*`, `I_*`, `TAC_*`); `mbx/core.c` (`mbxemu_reg_write`: what each
  write does, the MMU, contexts, free list); `mbx/ta.c` (the stream parser:
  `command()`, `hdr_words`, `hdr_block()`, the section/code/constant words in
  `ta_word()`, `vertex_words()`, `vertex()`, `project()` for the viewport and
  depth formulas, `ta_start()`, `ta_end_scene()`, `write_regions()`, `tile_grid()`);
  `mbx/vgp.c` (instruction layout in its header comment, the interpreter);
  `mbx/render.c` (`render_kick()` for the registers a render reads, `depth_pass()`
  and the state word bits near the top); `mbx/pb.c` (the parameter-buffer writer
  is off without a free list). The "o1 is BGRA" fact is in `mbx/ta.c`
  (`project()`) and `mbx/README.md` assumption 12.

### 2026-11-06: M2 step 2, the kernel path (`mbxtri-drm`)

- `mbx/drm` grew from a probe into a render node that can run a frame:
  - `mbx_mmu.c`: an address space a DRM file, 32 MB of 4 KB pages through
    the eight page tables (bare physical page addresses, README assumption 8),
    allocated coherent; pages with no buffer point at a scratch page, never
    physical 0. `drm_mm` places buffers from 64 KB up. The MMU holds one space
    at a time; a job switches it (tables written, which also flushes the TLB).
  - `mbx_gem.c`: shmem GEM objects mapped write-combined for the CPU
    (`BO_CREATE`), `BO_INFO` maps one into the file's MBX space and returns the
    device address and the mmap offset, `BO_UNMAP`. PRIME import goes through
    `drm_gem_shmem_prime_import_sg_table`, so omapdrm scanout buffers can be
    rendered into (untested yet).
  - `mbx_job.c`: `SUBMIT` is one blocking frame: TA registers, `0x800` start,
    the stream as chunks (type << 28 | count; TA words to the TA port, control
    words to the control port, by CPU PIO), wait for TA_COMPLETE, render
    registers, kick, wait for RENDER_COMPLETE. The interrupt handler clears
    the bits and wakes the waiter; TA_OFLOW and TA_STREAM end a wait as
    -ENOMEM and -EINVAL; a timeout or error resets the cores (`0x080` =
    `0x111`). One frame at a time under a mutex.
- Shared test code: `mbx/tools/tri_frame.h` builds the stream for both
  `mbxtri` (host emulator) and `mbxtri-drm` (guest, ioctls), so both run
  the same words.
- Tooling: `kernel/scripts/build-mbx-module.sh` (module-only rebuild, about a
  minute), `rootfs/scripts/stage-tools.sh` (cross-builds `mbxtri-drm` with
  Buildroot's host gcc into an overlay). A rootfs rebuild plus boot is about
  two minutes.
- **Result:** in the QEMU guest, `mbxtri-drm` found `/dev/dri/renderD129`,
  made four buffers, submitted, and drew 1144 of 4096 pixels; the render ran
  through the real kernel driver, the page tables, the interrupt and the PIO
  feed. With QEMU built from this repo (`qemu/prepare-tree.sh build/qemu-src
  --mbx`, then the docker build of NOTES.md), the guest's picture is
  byte-identical to the host `mbxtri` picture (all 12,301 bytes of the PPM).
- A first run used a QEMU binary from outside the repo and differed in 8 bytes
  (one colour step at four mirror-image pixel pairs): that binary embeds a
  different emulator core. Lesson: `scripts/run-qemu.sh` now defaults to the
  in-repo build (`build/qemu-src/build/qemu-system-arm`) and refuses to start
  without one; the guest tests only count against the repo's own emulator.
- Not yet: asynchronous submit and fences, the parameter buffer and free list
  (the model does without; the real chip needs it), partial render on
  overflow, DMA feeding, runtime PM, and the real N810.
- Sources: page-table format, address-space size, TLB behaviour:
  `mbx/core.c` (`translate()`, `R_MMU_*` writes) and `mbx/README.md` assumption 8;
  interrupt bits and the soft reset (`0x080`, bits 0, 4, 8): `mbx/core.c`
  (`soft_reset`, `emu_raise`); the power-up sequence: `mbx/kmod/gpl/driver/mbxaccess.c`
  (`MBXAccess_GfxClocks`); kernel APIs, read in `kernel/build/linux-6.18.58`:
  `include/drm/drm_gem_shmem_helper.h`, `drivers/gpu/drm/drm_gem_shmem_helper.c`
  (`drm_gem_shmem_get_pages_sgt`, `drm_gem_shmem_prime_import_sg_table`,
  `map_wc`), `include/drm/drm_drv.h` (`open`, `postclose`,
  `gem_prime_import_sg_table`), `include/drm/drm_mm.h`; the byte-identity check
  compares the guest's PPM with `mbx/mbxtri`'s. QEMU used for the guest check
  is built from `qemu/` in this repo (`qemu/prepare-tree.sh --mbx`,
  `qemu/docker/Dockerfile.9.1`).

### 2026-11-06: the VGP encoder (`mbx/lib/mbx_vgp.h`)

- A header-only encoder, decoder and disassembler for VGP instructions, with
  the hardware's rules built in (one constant register and one input register
  an instruction, sources from src0, 192 constants, temporaries 0-13, outputs
  0-7, register file 3 refused). Errors come back as `MBX_VGP_E_*`, so the
  compiler can report which rule a shader broke.
- `make -C mbx test` runs `tests/t_vgp.c`: every legal instruction it can
  generate (all opcodes with a reference: MOV, ADD, MUL, MAD, DP3, DP4, MIN,
  MAX, SLT, SGE, FRC, RCP, RSQ, EXP, LOG, DST; random registers, swizzles,
  negates, masks, constant windows and relative constants) is run through the
  model's own `vgp_exec()` and compared with a reference that works from the
  operand description, so a field in the wrong bit would show. 128,034 checks,
  no failures. Also: refusals, the disassembly text, AMV and `c[a0+n]` with a
  window. Not covered: LIT, EXPP, LOGP (no reference written yet).
- Next: the draw-state encoders (header blocks and the ISP/TSP state word,
  texture words, blend and depth state) as a library of the same kind, then
  Mesa.
- Sources: instruction layout, opcode list and numbering, the one-constant and
  one-input register rules, constant windows, relative constants and AMV:
  `mbx/vgp.c` (header comment, `enum`, `nsrc[]`, `read_sources()`, `vgp_exec()`,
  `vgp_run()`) and `mbx/README.md` assumption 13; output register roles:
  `mbx/ta.c` `vertex()` (o1 colour BGRA, o2 offset colour and fog in w, o3 and
  o4 texture coordinates, o5/o6 user clip planes enabled by sections-word bits
  20-27, o7.x and o7.y front and rear clip, always on) and `project()`; limits
  (192 constants, 256 instructions, 14 temporaries, 8 inputs): `mbx/emu.h`
  (`struct vgp`, `struct vgp_regs`). Not covered by the test: LIT, EXPP, LOGP.

### 2026-11-06: Mesa 26.1.8 builds with an mbx driver (`mesa/`)

- Buildroot 2026.08.1 ships Mesa 26.1.8; `mesa/scripts/prepare-mesa.sh` fetches
  it (sha256 checked), applies `mesa/patches/` and copies in `mesa/src` and the
  uapi header. `mesa/docker` builds the host image (`n810-mesa:1`, meson 1.12);
  `mesa/scripts/build-host.sh` configures and builds Mesa with only the mbx
  gallium driver, GLES2 and EGL (GBM, surfaceless), no LLVM. It builds and
  installs (`build/mesa-host-install`), and libgallium carries the driver.
- Patch 0001 hooks `mbx` into meson (options, default driver lists,
  `with_gallium_drm`), the DRM pipe loader and `drm_helper`, and both
  targets (dri, dril). Patch 0002 adds an `mbx` branch to kmsro: the KMS
  device (omapdrm) allocates the scanout buffer as a dumb buffer (contiguous,
  as its DSS needs) and the MBX imports it by PRIME into its own address space,
  which is what `mbx_vm_map` is built for.
- The driver is a stub so far: a screen with the VGP's limits as caps (256
  instructions, 192 constants, 14 temporaries, no control flow, two samplers)
  and a winsys that recognises the render node by `MBX_ABI_VERSION`; the context
  returns NULL. `MBX_ABI_VERSION` moved into the uapi header so the kernel and
  Mesa share it.
- 26.1 differences from older Mesa that matter here: caps are a `pipe_caps`
  struct filled at screen creation (`u_init_pipe_screen_caps`) and
  `shader_caps[]`, not `get_param`; uapi headers are in `include/drm-uapi`;
  `-Dgallium-vdpau` is gone (`-Dvideo-codecs=`).
- Next: a drm-shim backend (Mesa's `src/drm-shim`) that implements our ioctls on
  top of `libmbxemu` in-process, so the same DRM winsys code runs on the host;
  then a real `pipe_context`.
- Sources: the Mesa version, URL and sha256: Buildroot 2026.08.1's
  `rootfs/build/buildroot-2026.08.1/package/mesa3d/{mesa3d.mk,mesa3d.hash}`
  (the hash file cites the announcement
  https://lists.freedesktop.org/archives/mesa-announce/2026-August/000865.html;
  tarball from https://archive.mesa3d.org/mesa-26.1.8.tar.xz). The hook points were found by
  searching the 26.1.8 tree for how `vc4` is wired in: `meson.options`,
  `meson.build`, `src/gallium/meson.build`,
  `src/gallium/auxiliary/pipe-loader/{pipe_loader_drm.c,meson.build}`,
  `src/gallium/auxiliary/target-helpers/{drm_helper.h,drm_helper_public.h}`,
  `src/gallium/targets/{dri,dril}/*`, `src/gallium/winsys/vc4/drm/*`,
  `src/gallium/winsys/kmsro/drm/kmsro_drm_winsys.c`. The 26.1 screen and caps
  API: `src/gallium/drivers/vc4/vc4_screen.c` (`vc4_init_screen_caps`,
  `vc4_init_shader_caps`, `vc4_screen_create`); `include/drm-uapi/` listing;
  meson 26.1 option names: `meson.options`. Docker image: `mesa/docker/Dockerfile`
  (Ubuntu 24.04, pip meson, meson 1.12.1 reported by the image); the meson
  version requirement is `meson_version : '>= 1.4.0'` in Mesa's `meson.build`.

### 2026-11-06: the host sim (drm-shim)

- `mesa/src/gallium/drivers/mbx/drm-shim/mbx_sim.c` is a Mesa drm-shim backend:
  it implements the five mbx ioctls in userspace on top of the emulator core
  (copied into the Mesa tree by `prepare-mesa.sh`), with the same layout as
  the kernel driver: per-file page tables in the shim's memfd (physical memory),
  buffers mapped with a VMA heap, a submit that sets the MMU, the TA, feeds the
  stream, renders. `LD_PRELOAD=libmbx_sim_drm_shim.so` makes a program see an
  `mbx` render node, so Mesa's real DRM winsys and driver run on the host
  without a kernel or QEMU. (Mesa builds it with `-Dtools=drm-shim`.)
- Check: the guest program `mbxtri-drm`, built for x86 and run on the shim, draws
  the same picture as the host emulator directly (byte-identical PPM).
- Next: a real `pipe_context` for mbx: buffers over the ioctls, state objects,
  `draw_vbo` emitting the TA stream, then NIR to VGP.
- Sources: Mesa 26.1.8 `src/drm-shim/{README.md,drm_shim.h,device.c}` (the shim
  API, the 4 GB memfd and its heap, driver ioctl indexing at `device.c` line
  ~307, `drm_shim_driver_prefers_first_render_node` in `drm_shim.c` ~120) and
  `src/broadcom/drm-shim/{vc4_noop.c,meson.build}` as the pattern;
  `src/meson.build` (the order of `subdir('drm-shim')` before gallium); the
  ioctl semantics are this repo's own `mbx/drm/mbx_job.c` and `mbx_mmu.c`, and the
  emulator is `mbx/*.c`.

### 2026-11-06: the mbx pipe_context (resources, state, jobs, draw)

- The driver now has a context that records and submits frames: `mbx_bo.c`
  (ioctl wrappers), `mbx_resource.c` (buffers, linear 2D targets padded to
  whole 16-pixel tiles, depth buffers with no memory because depth is on chip),
  `mbx_state.c` (state objects as copies), `mbx_job.c` (the TA stream as
  chunks, one `SUBMIT` per flush, regions, tails and background buffers kept
  per target size), `mbx_draw.c` (the vertices read on the CPU into the stream
  after an object header, the VGP program and constants only when they changed
  in the job; triangle lists, strips and fans), `mbx_context.c` (flush, a
  CPU-filled colour clear, depth clear as the next render's on-chip clear
  value). It builds with the rest of Mesa on the host.
- Not there yet: the vertex compiler and fragment matcher, so `draw_vbo` logs
  "no program" and skips; textures; blending and alpha test words; scissor
  (planned as user clip planes in the vertex program); scanout through kmsro.
- Cull bits follow README assumption 4 (bit 19 culls negative area in surface
  pixels, bit 18 positive) combined with the sign of the viewport's y scale;
  not tested against a picture yet.
- 26.1 API changes met: `clear` takes `color_clear_mask` and
  `stencil_clear_mask`; `set_constant_buffer` and `set_sampler_views` have no
  ownership flag (the driver takes its own reference); `util_set_vertex_buffers_count`
  takes four arguments; the shader stage type is `mesa_shader_stage`.
- Sources: callback set and structure: Mesa 26.1.8
  `src/gallium/drivers/vc4/{vc4_context.c (vc4_context_create),vc4_state.c (vc4_state_init),vc4_draw.c,vc4_program.c,vc4_resource.c}`;
  current signatures: `src/gallium/include/pipe/p_context.h` (clear,
  set_constant_buffer, set_sampler_views, set_vertex_buffers, the sampler view
  release comment), `src/gallium/auxiliary/util/u_helpers.{h,c}`,
  `src/util/u_pack_color.h`, `src/gallium/auxiliary/util/u_debug_cb.h`; the
  compile errors that showed them are in `build/mesa-host.log`. TA stream,
  header blocks, state word bits, region array size, `0x6dc` and render
  registers: `mbx/ta.c`, `mbx/render.c` (`render_kick`, `depth_pass`), `mbx/emu.h`,
  `mbx/README.md` (assumptions 4, 5, 6, 7); the CPU-fill clear relies on the
  model reading the target's own pixels when no background object is set
  (`mbx/render.c` near line 1425 and README assumption 6), which is the model's
  behaviour, not seen on a device.
