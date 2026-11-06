# Journey: a modern GPU stack for the N810's PowerVR MBX

A running log of the work to give the MBX a from-scratch DRM kernel driver
and a Mesa Gallium GLES2 driver, aiming at an animated triangle on omapdrm,
first in QEMU and then on a real N810. Newest entries at the bottom. Facts
marked *unverified* have not been checked on the emulator or the device.
For the setup snapshot see `NOTES.md`.

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
