# mbxemu: a functional PowerVR MBX for QEMU

A model of the N810's PowerVR MBX1 (OMAP2420), built from observing how the
chip behaves on a Nokia N810: the register and TA-stream traffic the host
software sends it, and what it does in return.

## Layout

| File | |
|---|---|
| `mbxemu.h` | the public interface: a host gives memory, the IRQ line and a log; calls in with register and port accesses |
| `emu.h` | internals shared by the core's files |
| `core.c` | register file, interrupt status/enable/clear and the line, soft reset, MMU and TLB, device memory, the two TA hardware contexts (reset, store, load), the free-list build |
| `ta.c` | the TA: stream parser, object header blocks, VGP uploads, vertices through the VGP, primitive assembly, clipping, projection, culling; the region array at the end of a scene and at a close |
| `vgp.c` | the VGP interpreter |
| `render.c` | the 3D core: region array walk, background object, tile depth (on chip, or loaded from / stored to the external Z buffer), rasterizer, depth and alpha tests, texturing, offset colour, fog, blending, the colour buffer |
| `texture.c` | texture words A/B, texel formats, twiddled/linear/mipmapped addressing, nearest/bilinear/trilinear sampling, the word-C combiner and DOT3 |
| `twod.c` | the 2D core |
| `kmod/` | the MBX kernel driver source (GPL-2.0, `kmod/COPYING`), built against the 6.18 kernel by `../kernel/scripts/build-mbx.sh` |
| `../qemu/device/mbx.c` | the QEMU device (SysBusDevice), for 9.1 |
| `../qemu/device/install.py` | puts the device into a patched QEMU 9.1 tree |

## Build (x86)

```sh
cd qemu-n8x0/mbx
make                 # libmbxemu.a
```

## The QEMU route

**Decision: the patched QEMU `n810` machine (9.1.3), with the
MBX added at its real address.**

- Upstream QEMU no longer has the machine: `n800` and `n810` (with
  `akita`, `cheetah`, `spitz`, ...) were deprecated and **removed in QEMU
  9.2** (docs/about/removed-features.rst). The last release with them is 9.1.
- The n810 work runs on QEMU 9.1.3 + `qemu/patches/`
  (`qemu/docker/Dockerfile.9.1`, image `n810-qemu:9.1`): Linux 6.18 boots to a shell there with the RFBI panel,
  keyboard, touchscreen, eMMC and USB networking. That is
  the machine the MBX is modelled on: the registers are at `0x50000000`, the
  slave ports at `0x50008000`, the PRCM's GFX domain at `0x48008000` and the
  INTC line through the `ti,omap2-intc` domain, as on the N810.

The core has no QEMU dependency (`mbxemu.h`), so a second wrapper for a
newer QEMU is ~150 lines when it's wanted.

### The device

`qemu/device/mbx.c` is a SysBusDevice with one 64 KB MMIO region (registers at 0,
TA port at 0x8000, 2D port at 0xa000, TA control at 0xc000, 32-bit
accesses), one IRQ, and the MBX's memory accesses through
`address_space_memory`. `qemu/device/install.py` copies it and the core into a
patched QEMU 9.1 tree and changes
`hw/arm/omap2.c` in two places: the device at `0x50000000` on
`OMAP_INT_24XX_GUFFAW_IRQ` (line 21), and `RM_RSTCTRL_GFX` setting the reset
status bit `RM_RSTST_GFX` 0x10 that the guest driver polls for.

```sh
qemu-n8x0/qemu/prepare-tree.sh ~/qemu-mbx --mbx
mkdir ~/qemu-mbx/build
docker run --rm -v $HOME/qemu-mbx:/work -w /work/build n810-qemu:9.1 \
    sh -c '../qemu-9.1.3/configure --target-list=arm-softmmu --disable-werror --disable-docs \
           --disable-sdl --disable-gtk --disable-vnc --enable-slirp --disable-download && \
           ninja qemu-system-arm'
```

`-global mbx.trace=1` logs every register access, TA start, end and render
to stderr (2: every TA and 2D word too).

## Known gaps

- **Partial renders from the parameter buffer.** The model writes the
  parameter buffer as the N810 does (assumption 26), stalls with TA_OFLOW
  when the free list runs dry and closes as the N810 does, but renders
  from the scene it keeps in its own memory. The tail pointers (a cache the
  TA writes back when it likes) and the EVM contexts aren't written.
- **Line geometry.** A line's ends and binning aren't snapped as the TA does
  (assumption 26).
- **Stream-out's Y** beyond assumption 23's rule.
- **Terminate word 2** ("end block") alone leaves the scene without
  TA_COMPLETE.
- **The input FIFO**: 59 words shared by the TA and 2D ports (a TA hung by a sections word without bit 17 fills it: assumption 30),
  `INT_STATUS` bits 16-23 its fill, the port stalling the bus while it is
  full (so unpaced DMA works). The model takes every word at once.
- **Texture and plane arithmetic**: the set-up arithmetic of assumptions 15,
  18, 27 and 29 is a fit, not a derived circuit; some triangles (thin or
  near rounding ties) still differ from the device by a step.
- **2D**: pattern and mask surfaces, 1-bpp and RGB888 sources beyond the
  basic conversion; clip rectangles are modelled but not exercised.
- Register file 3 of the VGP (reads 0).
- **Timing**: everything finishes inside the access that starts it.
- **Migration**: the QEMU device has no vmstate.
- **Big-endian hosts**: device memory is read as little-endian words.

The N810 has no translucency autosort unit (core ID configuration bit 2 clear
in `0x01010003`) and draws objects in submission order, as the model does.

## Assumptions

Where the hardware isn't known, the model does what host software is assumed to do.
Each is marked in the code too. Items marked observed come from watching the
device; fitted ones reproduce observed behaviour without being a known
circuit.

1. **Screen-space objects** (`0xb00b0800`): positions are surface pixels and
   z is depth as sent: no perspective divide, viewport or depth transform.
2. **The end word vs. a stop**: `0xf0000000` ends the scene when 0x85c bit 7
   is set, and only stops the TA (the scene stays open, to be stored) when
   it's clear, as a stop word writes it. Both raise TA_COMPLETE.
3. **TA contexts**: a store (0x818) files the open scene under the TA context
   address in 0x820 with the TA's state blocks; a load (0x814) brings it back
   into context 0x810; a reset (0x81c) starts an empty scene there. The
   render reads the scene of the context in 0x6ec. What the hardware keeps in
   the TA context memory isn't known. The stored context itself is 65 words,
   a row of 8 per macro-tile row, each a macro tile's next free address as
   `0x00800000` | its word address, words 32-63 zero, word 64 the shared
   blocks' next free address (observed; written by pb.c's
   `pb_ctx_store()`). A load takes the entries back, a zero starting the
   macro tile on a fresh page, which is how software re-seeds the TA after
   an overflow's partial render.
4. **Culling**: primitive word bit 19 culls triangles
   with negative area `(b - a) x (c - a)` in surface pixels (y down), bit 18
   positive. This is the opposite of what the bit names suggest taken
   literally (CW vs. CCW); observed.
5. **Region array**: written at the end of a scene for every entry
   (`0x20000000 | y << 8 | x`, bit 25 for tiles without triangles, bit 26 on
   each macro tile's last entry, bits 31 and 26 on the screen's last tile)
   and at a close only for tiles with triangles, as a partial render's
   walk of the array expects. "Has a list" is the triangles' bounding boxes.
   The render walks the array at 0x608 to bit 31 and renders every entry it
   lists, empty or not (an empty one only gets the background); without an
   end mark within 16384 entries it raises nothing (the hang the watchdog
   expects).
6. **Tile start**: colour from the background object (the first words of the
   parameter buffer at 0x83c, texture words A and B: a linear texture read
   texel for pixel); the chip needs a whole object there (assumption 31):
   without one it flushes garbage, which the model shows in strict mode and,
   forgiving, reads as the colour buffer itself. Depth
   from 0x6dc (`0x3f800001`, taken as the clear depth 1.0) unless loaded.
7. **External Z buffer**: loaded when 0x610 bit 1 is set, bits 16-17 are
   clear and the entry has bit 29; stored only by a partial render (0x610
   bit 0 and 0x85c bit 3), observed. Layout: one float a pixel with the
   low bit set, rows of `((0x610 >> 8 & 0xff) + 1) * 16` pixels, at
   `0x674 & ~0xff`.
8. **MMU off**: device addresses are physical. (The stack always runs with
   it on; texture words can't hold a physical SDRAM address anyway.) The TLB
   is loaded from the page tables on first use and refreshed by 0x1020 bit
   8 with the page in 0x1024 and the physical page in 0x1028; enabling the
   MMU or writing a table base flushes it.
9. **Soft reset** (0x080 non-zero): scenes, stored contexts and the TA
   parser go, the MMU is off; VGP code and constants and other registers
   stay.
10. **Interrupt line**: level, up while `status & enable & 0x7fff` and
    enable's master bit 0x8000. INT_STATUS writes set bits.
11. **Slave port reads** return 0. The TA takes words at any lane and only
    counts them.
12. **Vertex inputs**: missing components are 0, w is 1; packed inputs
    unpack bytes in memory order to x, y, z, w (observed); o1 is BGRA and
    o2 is taken the same way (o2's order is assumed), o2.w the fog
    factor.
13. **VGP per vertex**: temporaries, outputs (o0 = 0,0,0,1) and a0 start at
    zero for each vertex; sections run in bit order, each `(flags & 15) + 1`
    times. The rules "one constant register" and "one input register" an
    instruction apply among the sources the opcode reads.
14. **Clipping**: o7.x and o7.y >= 0 and the enabled user planes >= 0,
    and no guard band: the hardware draws vertices as far away as 1e36
    pixels (observed), with no wrap or saturation of coordinates. What ends it
    is float32: beyond about 1e37 pixels the triangle is not drawn, and the
    model's float32 area and grid do the same. No clip at w = 0 and no
    near/far z clip: a vertex with w <= 0 projects as if 1/w were 0, to the
    viewport's centre (observed).
15. **Rasterization**: vertex positions rounded to sixteenths of a pixel,
    pixel centres at +0.5, top-left fill rule (observed). An exact tie at
    1/16 goes to the even sixteenth (observed). The VGP's viewport transform
    chops: x = trunc(trunc(trunc(pos * rhw) * vp0) + vp1), y likewise with
    vp3 and vp2, truncating toward zero after the multiply and after the
    add (observed with w = 1; other multiply and add orders for general w
    were not tested). The rhw in the product is the full-width truncated 1/w
    with its last mantissa bit cleared, 23 significant bits (fitted).
    Colours and coordinates are perspective-correct, depth linear in screen
    space (the depth is the exact edge values summed in double and rounded
    once, and a vertex z that rounds up to a power of two in float32 is that
    power of two before the 20-bit cut; the power-of-two rule is unverified
    at other binade edges); flat shading takes the last vertex sent; the tile
    colour is kept in float and rounded once when written (no dithering).
16. **Polygon offset**: the primitive word's low 16 bits, signed, times the
    depth block's third word (1e-5).
17. **Points and lines**: a square of the point size, a quad of the line
    width (no end caps); one set of attributes for a point.
18. **Mipmaps** (all of the rule below is fitted unless noted): the level of
    detail comes from the derivatives of s and t taken once per 16 x 16
    block, at its corner (x and y multiples of 16), as the numerators
    A_n q0 - n0 A_q, divided by each pixel's own q squared. The norm is
    max + (sqrt 2 - 1) min of each axis' s and t numerators, then the larger
    axis, multiplied by 1/q^2 through the device's 8-bit reciprocal path:
    R1 = 1/q with its significand floored to 8 bits (the ISP's reciprocal
    ROM, rcp16); S = R1^2 floored to 8 bits = (rho / 256) * 2^e, rho in
    128..255, eps = (rho - 128) / 128; the multiplier works on V * 2^t with V
    the length's significand in [1, 2) and gives (V + r) / 2 * 2^t, r =
    floor7(floor7(V) * eps), floor7 = floor to 1/128. eps = 0 is exact, so
    q = 1 is. The multiplier circuit itself is not known (the rule fits
    about 99% of samples).
    The log2 is piecewise linear (the exponent plus the mantissa less one;
    observed that exact log2 doesn't fit), summed in double and converted to
    float cut toward minus infinity, not rounded. The LOD's q, s and t plane
    constants are rounded to 16 bits with ties away from zero, and the s, t
    and 1/w (q) planes' slopes each take their own 16-bit exponent in the
    derivative block, for every primitive; the pixel's own q for the 1/q^2
    cuts comes from the shared-slope planes of `plane_at`. The smallest
    level is word A bits 9-12's. Word B bits 25-28 are the D adjust (bias =
    field / 4 - 1.75 levels; nearest-filter bits make +0.5, trilinear
    ones 0), which the model takes as given by those.
    Trilinear blends the two levels around it unrounded, in 64ths
    truncated (the top bin, 63/64 and up, taken as 64/64: the pure next
    level), the near level's term (byte x (64 - fraction)) first cut to a
    multiple of 32, the far term exact: out = (((a (64 - kf)) >> 5 << 5) +
    b kf + 32) >> 6 (observed). Nearest takes the level as (int)(lod + 0.5) of
    the piecewise-linear log2 with the threshold moved: the level changes
    where the fraction passes 27/64 (0.4219), not 0.5 (fitted; the exact
    circuit is a guess).
19. **Texture formats**: word A bits 14-16, 0 ARGB1555, 1 RGB565, 2
    ARGB4444, 3 ARGB8332, 4 LA88, 5 L8/A8, 6 ARGB8888, the 3, 4, 5 and 6
    bit fields replicated to bytes; bit 17 makes it PVRTC, 2 bpp with
    format 0, 4 bpp with 6, decoded as Imagination's published
    decompressor with translucent colour B's blue expanded properly. Format 7
    alone (word A bit 17 clear) is one bit a texel, set opaque white, clear
    transparent black, each byte 8 texels of a row with the leftmost in bit
    7, the bytes twiddled over (x / 8, y), never filtered.
19b. **Texture address**: word B bits 0-17 (device address / 128); linear when
    the stride field (word A bit 0, word B bits 18-24, in eights) isn't
    zero; mipmapped by word B bit 29 and trilinear by bit 30 only for
    twiddled textures. A linear texture with bit 29 set (`0x2e000000`) is read with its rows the width apart, whatever its stride
    (observed).
20. **Combiner**: products clamped, then less 0.5 and the scale, the result
    clamped; DOT3's D code 1 and 3 scale by four, 0 and 1 also write alpha.
21. **2D colour key**: a source key skips matching source pixels, a
    destination key writes only over matching ones (assumed).
    The solid colour is in the destination's format. ROPs work
    on the destination format's bits; alpha blends in 8-bit ARGB after the
    ROP. Clip rectangles have exclusive right and bottom.
22. **Free list**: the table is written (`entry[p] = p + 1`, entry 0 the
    head, the last entry itself) and EVM_DALLOC raised; the model's
    parameter memory doesn't come from it.
23. **VGP stream-out** (header blocks 9-11), as the N810 does it
    (observed): the address word is ignored. An object whose header has the
    three blocks and a non-zero enable writes each vertex's selected
    outputs, after the VGP and before clipping, in register order (o0 and o5
    four floats, o1 and o2 four clamped to 0..1, o3 and o4 s and t), from one
    of two places that alternate per such object: X, device `0x20c`, and Y.
    The side flips after each object that streamed and at each render. Y is
    device 0 the first time after a render, `0x202e00` after it was written,
    and nowhere (dropped) after a reset, where the side starts on Y. The
    N810's Y also depends on what earlier objects wrote (unmapped pages after
    long X runs, `0x1000` for a scene's second object); that isn't modelled.
24. **Fixed-point colour arithmetic** (observed): blending works on 8-bit
    colours with factors in 256ths (ONE 256, 1 - x as 256 - x, the colour and
    alpha values as they are) and shifts the sum right by 8; the offset
    colour is added in 8 bits; fog is `(c * F + fog * (256 - F)) >> 8` with F
    one more than the factor's 8 bits (inferred from a single sample). In
    blend factors 255 counts as 256 (alpha 255 over anything is the source).
25. **Colour precision** (observed): a vertex colour is kept as a float of 8
    significant bits, truncated, then rounded to a byte at the vertex, before
    it is interpolated. Colours are perspective-correct, with 1/w truncated
    rather than rounded. An RGB565 target takes the 8-bit colour's top bits
    (assumed). The combiner (word C) works in 8 bits: 255 counts as 256
    (one), 1 - x is 256 - x, each product `(x * y) >> 8` truncated on its
    own, the two added, less 128 for the half, then scaled and clamped; DOT3
    sums `((2a - 256) * (2b - 256)) >> 8` a channel, rounding down. The
    environment colour's 5- and 6-bit fields expand by repeating their top
    bits (24 is 198). Bilinear filtering weighs the four texels in 32nds of a
    texel, the fraction truncated, and truncates the weighted sum of their
    bytes. Texture coordinates: assumption 27.
26. **The parameter buffer** (pb.c), as the N810's TA writes it (observed).
    Pages come off the free list (its table, from `0x828`'s page) as needed,
    and a scene goes on from where the last one stopped unless the list is
    rebuilt. A draw's vertices go out in blocks of up to six (two list
    triangles, or a strip's); the scene's first block goes once to the first
    page taken, every later one into the page of each macro tile the bounding
    boxes of its triangles (in tiles, ends included) touch; a macro tile's
    bump pointer hands out the block, then 16-word list blocks (a tile's
    first, or when its list has 15 entries; a link `0xe0000000` | byte
    address); what doesn't fit goes at the start of a new page. A fan goes
    out as strip blocks (rim, rim, centre, rim, rim; the state word's bit 6
    set), a point or a line segment as a 4-vertex strip block of its own; the
    region array gets each tile's list.
    Out of pages: the free list's last page (its entry pointing at itself) is
    never handed out; the TA raises TA_OFLOW and takes no input (the model
    keeps it, in order) until a TA_START (software's resume, `0x804` then
    `0x800`), then bins the block it had in hand on from where it stopped,
    from the list as it now is; the first new block after any TA start goes
    to the shared page, after the blocks already there (a new page when
    full). A close while stalled (a partial render) finishes the macro tile
    the TA ran out in, with the list's last page. A render links each macro
    tile's pages back on the free list after its last page (the table's entry
    0 the page last handed out); the tiles' lists start over (software
    clears the tail pointers), but each macro tile's next free address and
    the shared page carry on.
    Not the N810's: a line's quad. The TA snaps a line's ends (the major
    axis to half pixels, nudged 1/16 back on a half; the minor by a rule not
    found) and bins its triangles by something tighter than the box; the
    model offsets the ends by the half width unsnapped and bins by box, which
    matches only lines whose ends lie on the grid. A point's square is moved
    by (1/32, -1/32), observed at size 4 only.
27. **Texture and colour planes** (render.c `persp_attr`, `plane_of`,
    `plane_at`, `vterm`): 1/w and s/w, t/w are planes, A x + B y + C. The
    arithmetic follows how the device appears to narrow its values; the
    exact rule is fitted, not a known circuit. What the model does:
    - *Vertex terms.* Each vertex's 20-bit 1/w is multiplied by the edge
      constant first, cut to 24 bits toward zero, then by the coordinate and
      cut to 24 bits again: cut(cut(rhw * E) * s). Where all three vertices
      lie on half pixels, the s/t planes' products are cut to 24 bits before
      the chopping add (a half-pixel gate, fitted).
    - *Edge constants and area.* The edge constants x_i y_j - x_j y_i are
      exact integers in 1/256 px^2; each product is cut to 24 bits toward
      zero, then the difference to 24 bits again (`edge_k`). The area used for
      the reciprocal is the sum of the three cut edge constants, not the exact
      cross product. The reciprocal is trunc19(1/trunc19(area)) for the
      texture planes (the colour planes keep the uncut area where they differ).
    - *Constants and slopes.* C is formed by Cramer's rule: each vertex's
      value times its opposite edge's constant, with no reference vertex,
      summed through a 24-bit sign-magnitude accumulator that chops every
      add's result toward zero (the first term's load too). The slopes are
      the edge sums, accumulated the same way, times 1/area. The three terms
      are added in submission order: the vertex the TA received last is added
      last, the other two by x descending, for every area. For an area of
      more than 19 bits the 1/w plane's chop grid is never coarser than
      2^(e_final - 24), e_final the exact sum's exponent. The 1/w plane runs
      through the same accumulator as the s/t planes.
    - *Rounding of stored values.* Plane constants C are rounded to 16 bits
      with ties away from zero; slopes are rounded to 16 bits and stored with
      one exponent for the pair (the larger one's; a zero slope takes no
      part), each floored to 16 bits below it; the slope grid is 2^(ec-27)
      with ec taken from C as rounded.
    - *Per pixel.* The plane value at the 16-pixel block start is built from
      C and the slopes with products cut to 24 bits (the block start sum
      floored at 27 bits, the two products keeping 26), then A is stepped
      across the block; the value is cut toward minus infinity at 16 bits. The
      reciprocal of 1/w is the device's ROM (rcptab.c), a table
      fitted entry by entry, not a derived circuit. The coordinate is (s/w) *
      w kept to 12 bits below the two factors' exponents together and rounded half up: 12 or 13
      significant bits, 13 where the significands multiply past 2. A negative
      power-of-two s/w takes one more bit on the s*(1/q) product grid. When
      1/w is the same at every vertex, the 1/w plane's C stays that 1/w
      truncated.
    - *Colours.* Colours go through the same planes, in bytes (the vertex's
      byte times 1/w). A perspective triangle (1/w differs between its
      vertices) sets up the colour channels and the colours' 1/w plane through
      the whole texture chain, the vertex value being cut(rhw * byte, 24, 0);
      an affine triangle sets up its channel C through the same accumulator
      with its slopes and 18-bit vertex value as the texture planes' affine
      case. The fog plane (colour channel u 1, c 3) carries 255 - byte through
      the unchanged RGB chain.
    - *Coverage.* The coverage test uses the edge constant with each product
      cut to 24 bits, A*x + B*y + edge_k, plus a bias of 2^-14 times the
      edge's larger slope on a top or left edge, taken off the others (the
      size and the diagonal edge rule are fitted).
    - *Object-type bits.* A primitive with both object-type bits set (state
      0x...6e80) draws nothing but claims the pixels it covers after the depth
      test; later primitives skip them. The last such primitive of a tile, if
      untextured, also sends part of its last scanline (x%16 >= 8 and the 8
      pixel groups that continue a run) back to the tile start colour: a
      covered pixel on y%16 == 15 is erased at x%16 >= 8, and at x%16 < 8
      exactly when pixel x0+8 (x0 = x&~7) is not covered by that primitive.
      What an erased pixel holds on the device is not modelled (it is the old
      pixel ANDed with a word that varies and looks like hidden state outside
      the TA stream); the model keeps the old pixel.
    Known gaps: thin, nearly flat triangles and some colour planes near
    rounding ties still differ from the device by a step, as do some
    perspective textured quads.
28. **Sampling points** (render.c, `0x61c`): bit 0 puts the coverage test
    (the set-up FPU) at the pixel's centre, clear its top left corner;
    bit 1 does the same for the shading and texture planes (the TSP); bit
    2 is the half texel taken off texture coordinates before bilinear
    filtering. Bits 16-17 (the depth plane's reference point) are not
    modelled. Software writes 0x00020007.
29. **Depth plane** (render.c `zplane_setup`, `zplane_tile`): the depth of a
    16x16 tile's first pixel (pixel centre xs = x0 + .5, ys = y0 + .5) is the
    floor to the 20-bit significand of N * rc, N = the 24-bit chop of
    cs + na xs + nb ys, where cs, na, nb are the texture planes' set-up sums
    of the three vertex depths (every product cut to 24 bits, the sum chopped
    to 24 bits after each add; the vertex with the largest y, among equal y
    the largest x, added last) and rc = trunc21(1 / trunc21(area)), the area
    being the sum of the cut edge constants. Inside the tile the depth is that
    start plus A dx + B dy with A = na rc and B = nb rc cut toward zero to
    2^-6 of the start's 20-bit unit, floored to the 20-bit significand of the
    sum. A triangle whose three vertex depths are equal is exactly that depth.
    The widths (21 bits for the area and the reciprocal, 24 for the numerator),
    the flat rule and the +-0.5 pixel reference (bits 16-17 of 0x61c, not
    modelled) are fitted.
30. **The sections word's bit 17** (observed on an N810, by a probe the user ran and
    reported; not seen by the model's author): the word after a primitive
    (`0xf0020000 | sections`, or `0xb00b0800` for screen space) needs bit 17
    (`0x20000`) set. With it clear the TA takes nothing: `INT_STATUS` read `003b0001`
    (bits 16-23, the input FIFO's fill, 59: full; and bit 0, which this model does not
    set and whose meaning is not known), no `TA_COMPLETE` came, and the render after
    it was not done after a second. The model hangs the TA at such a word: it takes no
    more words (each one waits in the FIFO, the fill rising to 59), raises nothing,
    leaves the scene as it was, and a render kick does nothing (no `RENDER_COMPLETE`);
    a TA start (`0x800`) or a soft reset gets it going again. What bit 17 selects is
    not known, and bits 16, 18 and 19 are still not looked at.

31. **What a client must do before a scene** (found during device testing on an N810; the
    model checks it in `ta.c` `check_contract()` and `render.c`, counts each miss in
    `mbxemu_stats.violations` and logs it once a scene as `contract: ...`; `lib/mbx_contract.h`
    does all of it for a client). The chip needs:
    - the free list: `0x824` the table (16 KB aligned), `0x828` the first page, which is the
      buffer's base page plus one (page 0 is the background object's), `0x82c` the last,
      `0x838` = 1, `0x83c` the buffer's base, `0x6d8` = `0x09000000`, `0xce0` = 1;
    - the TA and EVM contexts: zeroed, 16 KB aligned and padded (the chip writes past their
      nominal sizes: a page, and 520 bytes), loaded with `0x820` and `0x864`, `0x810` = 0 and
      `0x818` = 1 before a client's first TA (the model calls `0x820` the TA context memory and
      `0x864` the EVM's);
    - the background object, a whole object in the buffer's first words (the chip reserves
      132 words for it), 26 words: `e0000000`, texture word A (`a8800000` with the format: `0x4000`
      RGB565, `0x18000` ARGB8888), word B (`0x0e000000` with the stride in pixels / 16 in bits
      18-24 and the target's address / 128), word C (`d6887610`), the state word `22220e80`, three
      vertices as 6 position words (the far corners, a triangle twice the padded size, patched at
      words 7 and 10), six 1.0 (z and 1/w), nine words of colour and texture coordinates
      (patched at 21 and 25, in 1024ths): a texture of the render target itself, 2048 x 2048
      clamped, so a tile starts from the pixels the target holds;
    - the region array initialised: every entry `0x22000000 | the macro tile's origin (y << 8 |
      x)`, word 1 clear; the TA clears bit 25 and writes the list address of tiles that get one;
      and bit 30 (`0x40000000`) on every entry before the render (the stock daemon does it);
    - the tail pointers (`0x840`) 64 KB aligned and zeroed: the TA rounds the address down and
      uses what is there; at a 16 KB aligned allocation it zeroed the colour buffer's last rows
      (about 7 KB); a wrong value can reach kernel memory;
    - unmapped device pages pointing at one scratch page; the render's hardware context `0x6ec`.

    Not known: which miss causes which effect. With all of it missing (what the first runs of the
    driver did) the chip flushed noise (mostly ones with single bits cleared) in every tile with
    an object and, once, wrote into the kernel's page tables. With all of it done by the driver
    the page tables stay intact, but a tile with an object still comes out as the same kind of
    noise. A scene with no objects leaves every tile without a list untouched (the chip does not
    draw the background there: assumption 5 says the model does) and processes only the last
    region entry, the one with the end mark, which came out as noise with the whole background
    object and unchanged with its texture address moved.
    In strict mode (the default) the model gives white in the tiles that hold objects when any
    check fails, and white as every tile's start when the background object is not whole
    (instead of the target's own pixels, which assumption 6 used to say and the chip does not
    do); `mbxemu_set_strict(e, 0)` restores the forgiving behaviour and still counts. The model's
    render still draws from the TA's scene in memory and does not read the buffer's lists or
    objects, so a wrong list or object word is not seen.

    More of the contract, in the model since (all found during device testing; amounts marked
    "assumed" are guesses that fit one figure):
    - the tail area: a pointer not 64 KB aligned is used rounded down, and the TA zeroes the area
      there (`ta.c` `tails_clobber()`: assumed 7 bytes for each entry of the grid's bit-interleaved
      index range, 7 KB at 400 x 240, the one figure seen);
    - the contexts: 16 KB aligned, and the EVM context is written past its 520 bytes at the
      scene's end (assumed 1 KB), so what lies behind it is zeroed: pad it;
    - lists in memory: the render checks each listed tile's list (a block of 16 entries, the 16th
      linking on, `0xf0000000` ending it) and a damaged one (an empty entry before its end word,
      no end word) gives garbage. The chip's ISP reads them; the model still draws from its
      scene and only checks them. A TA stream without its end word never completes (no
      `TA_COMPLETE`), which was always modelled;
    - a render kicked while the scene is still streamed (no end word, no close) is counted
      (`V_RENDER_EARLY`); the model cannot show timing, so this is the only way a client's
      early kick appears;
    - after a partial render the free list is damaged; the next partial render hangs the core
      (no completion) unless the list is rebuilt with `0x838` (`V_FL_STALE`);
    - a write through a page no table entry maps lands on physical page 0: unused pages must
      point at one scratch page (`V_STRAY`);
    - an empty tile's region word 1 keeps what it held (the TA writes word 1 only for tiles that
      get a list).

    Not modelled, for want of what the chip does: a mismatch between `0x860` and the tile count
    in `0x85c` (found on the device to make the deallocation free the wrong pages and hang the
    core; the model has not used `0x860` and its encoding is not known); the TA input FIFO's
    59 words and the timing of anything (the model finishes inside the register access); the
    services' TA-busy ordering (a services-side hang, not the chip's). Unexplained on the
    device, no counterpart here: rendering into an oversized buffer came out black; stream-out's
    second place (Y) is not predictable (assumption 23); the depth of a partial render's lost
    macro tile.
32. **Interrupt bits 0 and 3** (seen on an N810 with the kernel driver, through `INT_STATUS` snapshots in
    `docs/n810-trace.txt`; the meanings are not known). A render's completion shows `INT_STATUS`
    `0x4d`: bit 2 (RENDER) and bit 6 (EVM_DALLOC), as modelled, and also bit 3 (the ISP) and bit 0;
    a context store (`0x818` = 1) shows bits 8 (TA_CONTEXT) and 0. The model raises them so; nothing
    here uses them (a driver that enables neither sees only the others). The chip also writes
    `0xcc0`-`0xce4` after a job: `0xcc0`..`0xcd8` the same in every job
    (`0c1033c2 29476053 d9f7855c badff4e3 6f3bdd1e 08a8822a 00000001`), `0xce4` different each
    job (`d631b1e3`, `754d38ee`: a signature or counter); the model has only the counter at `0xcc4`.
