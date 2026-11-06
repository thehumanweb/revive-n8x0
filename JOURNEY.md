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
