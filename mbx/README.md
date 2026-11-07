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

    Checks added from a second list (all counted and logged; the **soft** ones, marked `V_SOFT` in
    `emu.h`, are from the stock stack or GLES and not confirmed on the device, and never garble the
    picture): the contexts `0x820` and the free list table `0x824` 16 KB aligned and `0x844`,
    `0x864` 4 KB (soft); `0x85c`'s tile grid agreeing with `0x84c` and `0x850` (soft); `0x85c`'s base
    bits `0xe1` (soft; the driver writes them since); the free list chaining from `0x828` to `0x82c`
    (soft); the render's format, address and row (`0x650`, `0x65c`, `0x660`) describing the same
    target as the background object's texture words (soft); the set of render registers written
    since the last kick (soft): `0x608 0x60c 0x610 0x614 0x618 0x61c 0x63c 0x644 0x648 0x64c 0x650
    0x654 0x658 0x65c 0x660 0x674 0x6d0 0x6dc 0x860 0x680`, and `0x854`, `0x858` before the TA start.
    The driver and the tools do not write `0x60c 0x614 0x618 0x644 0x648 0x64c 0x6d0 0x854 0x858`
    or `0x860` yet because their values are not known here, so those misses are counted on every
    frame; a region entry with neither a list address nor bit 25 is hard (garbage). The write watch
    (soft) logs a bus write by the TA or the render outside the target, the parameter buffer and its
    table, the region arrays, the tail block, the contexts and the Z buffer, with its address.
    Not done: a cross-check of each list's objects in memory against the model's scene (it would
    call for the model's page layout, assumption 26, to be the chip's).

33. **The flow of a frame** (found during device testing; the stock stack's, which the model, the kernel
    driver and the tools now follow where the values are known).
    - Once, at power-up: reset and read `0xf00` (`0x01010003`); the MMU (eight page-table pages
      at `0x1000`-`0x101c`, `0x1020` enables it, every unused entry on one scratch page); the
      parameter buffer (N pages and a spare, page 0 reserved for the background object); the
      buffer filled with `0xf0000000` end words, the 26-word background object into page 0; the free
      list (`0x824` table, `0x828` the buffer's page + 1, `0x82c` last, `0x83c` the base,
      `0x6d8` = `0x09000000`, `0xce0` = 1, `0x838` = 1, then wait for EVM_DALLOC); the contexts
      (`0x864`, `0x820`, `0x810` = 0, `0x818` = 1).
    - Once per render target: the tile grid (16 x 16 pixels in macro tiles), two region arrays (one
      8-byte entry a tile, grouped by macro tile, each `0x22000000 | origin y << 8 | x`, word 1 = 0),
      the tail pointers (64 KB aligned, zeroed), the contexts (a page and 520 bytes, 16 KB aligned and
      padded).
    - Each frame: take the TA and one of the two region arrays; update the background object's format
      and address words for the colour buffer; queue the render's 20 registers (`0x608 0x60c 0x610
      0x614 0x618 0x61c 0x63c 0x644 0x648 0x64c 0x650 0x654 0x658 0x65c 0x660 0x674 0x6d0 0x6dc
      0x860`, then `0x680` = 1, the kick, last; the render waits for the scene); zero the tail pointers
      (stale ones make some macro tiles render); the TA: `0x85c` the grid and the base bits `0xe1`,
      `0x840` the tails, `0x844` the regions, `0x84c` and `0x850` the size, `0x854` and `0x858` =
      `0x3727c5ac`, `0x800` = 1; the stream; the end word `0xf0000000` after releasing and taking the
      TA again.
    - The stream: the region clip control (`0x10000010`, 0); a depth-only clear to the far plane (a
      screen-space object: without it the tile's depth reads as NaN and most depth tests fail; the
      model gives NaN, `V_DEPTH_CLEAR`); the draws, each with a header (`0x10000000 | mask`: the state,
      alpha test, texture units, vertex layout, viewport and depth transform blocks GLES sends), a
      primitive word, the sections word (`0xf0020000 | mask`, bit 17 needed) and the vertices (x, y, z,
      1/w, packed colours).
    - The chip: the TA bins each triangle into the lists of the tiles it touches, from the free list,
      writing vertex blocks and 16-word lists ending `0xf0000000`, filling in each region entry's list
      address and clearing bit 25. The kick draws, for each tile of the region array, the background
      object first (a screen-sized triangle textured with the target itself, so uncovered pixels keep
      their value), then the tile's list, to the last entry's end mark; it raises RENDER_COMPLETE and
      EVM_DALLOC and returns the tile pages. (On the unit the render skipped entries without a list
      and did not draw the background there; see the journal.)

34. **The MMU protocol and what the render flushes** (found during device testing).
    - The stock daemon's MMU sequence: soft reset `0x080` = `0x111` then 0, sixteen times, with interrupts
      off and cleared and `0x85c` = `0x80`; every page-table entry on one scratch page; the table addresses
      to `0x1000`-`0x101c`; `0x1020` bit 0 cleared and bit 16 read until it clears, then bit 0 set and bit 16
      read until it sets; then, for every page mapped, the table entry (a bare physical address) and a TLB
      load (`0x1024` the device page, `0x1028` the physical page, `0x1020` bit 8, bit 24 read until it
      clears). A 256-entry TLB is described elsewhere; whether the chip also walks the tables on a miss is
      not known. The model walks them on a miss and counts, softly, a page used without a software load
      (`V_TLB`) and the enable without the handshake (`V_MMU_SEQ`); the sixteen resets are not checked.
      The driver has switches for all three (`mmu_seq`, `tlb_mode`, `reset16`); with them, on the unit, the
      output is unchanged. Not modelled: the device's memory is all uncached (the target, the parameter
      buffer, the region arrays, the tails, the contexts and the free list's table); a cacheable buffer the
      CPU fills would need explicit cleaning, which the stock flow never does.
    - What the unit flushes: with our setup, every tile of a frame comes out as one of two 16 x 16 patterns
      (by tile parity), almost fixed from run to run (4048 of 4096 pixels the same in ten runs, about 40 vary),
      independent of what the target held, and each pixel is ones with about 2 of 24 bits cleared
      (`docs/n810-tile-flush.txt`). That is the look of the tile buffers (two, used by turns) being flushed
      with nothing drawn into them. The model gives white in such a case (strict mode).
35. **The parameter buffer is at device address 0** (found on the N810 with our driver). With its base at
    `0x1c00000` and at `0x100000` every tile that was processed came out as garbage (assumption 34); with the
    buffer at device address 0, its pages in the first arena (0-8 MB, here 768 pages), the free list's table and
    the TA and EVM contexts at `0x800000` and the clients' buffers after them, the render draws: the same
    shape as the model (1054 of the model's 1144 pixels, the same bounding box, no pixel the model does not
    draw) and, in the rest of the target, the clear colour kept. A list item's pointer is a word offset from
    `0x83c` (`0x40800500` for an object at base + `0x1400`), and the region entries' list addresses are
    absolute, so the two agree only at base 0; which of them the ISP applies the base to is not known. The model
    now counts a base other than 0 as a violation (`V_PB_BASE`, garbage in strict mode). What the chip draws is
    not the model's colour yet: 973 of the 1144 triangle pixels equal the expected colour ANDed with `0x20` (the
    colour the target was filled with), channel by channel, which reads as a bitwise logic operation against the
    old pixel; the others are partly exact, partly ANDed. Which register or state word selects it is not known.

36. **The state word and the alpha word** (found during device testing, parameter buffer at device 0). The
    triangle's colour is exact (1144 of 1144 pixels, the model's picture, no pixel differing) with the state
    word `0x22400200` (bit 25 set, source factor ONE, destination ZERO, Gouraud, depth LESS) **and the alpha-test
    word `0xe0000000`** (function ALWAYS) after it. The alpha word's function is in bits 29-31 and a zero word is
    function NEVER: with bit 25 of the state word set the unit honours it and rejects every pixel (nothing drawn),
    with bit 25 clear it is half applied: the footprint stays but the colour is the model's colour **ANDed with
    what the target held** (1000 of 1144 pixels; with ALWAYS and bit 25 clear 178). Source factor ZERO writes
    black. The model applies the alpha function when bit 25 is set even with bit 14 clear. The contract checks: a colour-writing draw with bit 25 clear is a soft violation (`V_STATE25`, counted: the unit's colour path differs); a NEVER alpha function with bit 25 set draws nothing and is not a violation, it is what the unit does. Not required: base bits on every state word (the clear object's state word does without). The clear object
    takes the same alpha word (`0xe0000000`; its state word's base bits make no difference). Registers: every
    bit of `0x648`, `0x64c` and `0x6d0` flipped one at a time changes nothing; bits 0 and 1 of `0x644` and bits
    12-25 of `0x60c` change which pixels are drawn; the render format `0x650` values other than `0xe` (and
    `0xd`) give garbage. Without a depth-only clear object nothing is drawn (the NaN depth).


37. **What a client does before every scene** (found during device testing; the driver does it, `mbx/drm/`). The TA
    context is reset for each scene: `0x810` = a hardware context id (0 or 1, alternating between scenes, the same
    value in `0x6ec` at the render), `0x81c` = 1 (an empty scene in that context), wait for `TA_CONTEXT` (bit 8 of
    `INT_STATUS`), clear it, then `0x800` = 1. The TA and EVM contexts are zeroed and loaded before every scene, not once
    (the EVM context holds each macro tile's list state: left over from the last scene, the TA goes on in pages the
    render has given back, and scenes of several macro tiles then hang or draw wrong tiles at random). The MMU's enable
    handshake (`mmu_seq`) is done whenever the address space changes: without it the TLB keeps the previous client's
    pages and one client's job renders into the other's buffers. With all three on the unit, 52 of 52 runs of 256x256,
    400x240 and 800x480 draw the model's pixel counts, `gbm_tri` runs 12 frames on the panel's 800x480 target with no
    failed job, and two clients at once give each its own picture. Not needed: a delay between `TA_COMPLETE` and the
    kick, rebuilding the free list per scene, bit 18 of `0x85c`.

38. **Texturing a draw** (found during device testing; `mbxtri-drm -N`, `mbxtri -N`). One texture unit is state word bits 16-18 = **2**
    (what the stock background object has); 0 is none, 5 two units; 1 draws nothing, and 3, 4, 6 and 7 hang the job
    (the model flags 1, 3, 6 and 7 as `V_TEXMODE`). The object header names the unit with mask bit 2 and sends texture
    words A, B and C after the alpha word; the vertex program writes the coordinate to `o3.xy`, the vertex layout names it
    (a third attribute of two floats). A linear 8 x 8 ARGB8888 texture (word A `0xa0018001`: clamp both ways, format 6,
    stride 8 texels in bit 0; word B `0x0e000000` | address / 128; stride 8 means field 0 and bit 0 of A) in four quadrants, drawn
    with the stock replace combiner (`0xd6887610`), with a modulate word (`0x86884610`: texel in A, primary colour in B,
    C = 1 - 1), and with bilinear (bit 31 of B), alone and with a rotation: **the picture is the model's, every pixel**
    (0 of 4096 differ on the unit).


39. **Texel arithmetic on the chip** (found during device testing, `mbxtri-drm -N ... -Q lo:hi -V fmt -H`, compared with `mbxtri`
    pixel by pixel; every case below ends with 0 differing pixels). Colour fields of a 16-bit texel are **shifted up, not
    replicated**: white RGB565 reads (248, 252, 248), white ARGB4444 240, white ARGB1555 248 (a 4-bit alpha is scaled).
    **Bilinear filtering is skipped when any of the four texels has R, G and B all zero**, whatever its alpha (ARGB8888
    `0xff000000`, `0x80000000`, `0x00000000`, 565 `0x0000`, 4444 `0xf000`, 1555 `0x8000`, LA88 with L zero): the sample is the
    first texel, (u, v) floored after the half-texel shift, so a black-to-white edge is a step where a white-to-grey
    one is a ramp. A texel of colour 1 in any channel filters normally. Checked: formats 0, 1, 2, 4 and 6, strides 8, 16
    and 32 texels.


40. **Mipmaps on the chip** (found during device testing; `mbxtri-drm -N 2 -U 5 -u scale -H bits` against `mbxtri`, a 32 x 32 twiddled ARGB8888
    chain whose levels are flat colours, drawn with the texture coordinates scaled so that 1 to 12 texels fall on a pixel). The
    chain is the model's: twiddled, the largest level first, no padding, the level count in word A bits 9-12, no stride. Word B
    bit 29 makes it mipmapped (nearest level); **the level bias field in bits 25-28 must be 9 for that** (`0x32000000`,
    the stock stack's nearest setting; 7, which I had from the linear background object, picks a level too fine at every ratio):
    with 9 the chip picks the level the model does at every scale tried (1 to 24, 0 differing pixels), alone and with the
    bilinear bit (`0xb2000000`). The chip's level is `floor(lod + field / 4 - c)` with c a little under 1.83. **Bit 30
    (trilinear) hangs the job unless bit 29 is set too, and with both set (`0x6e000000`, `0xee000000`, any field 7-10) the
    chip samples level 0 at every scale**, for every level-bias field 0-15 and at three scales; also level 0 with any single state
    word bit 20-21 or 23-28 added, any single word A bit 1-8 added, and a hang with state bit 19: the trilinear mode needs something
    not found yet (a second texture word or unit is the untried guess), so the driver uses the nearest level for the
    `_MIPMAP_LINEAR` filters. A texture that is both mipmapped and a render target is twiddled and the chip cannot
    render into it (the driver draws to it on the CPU).


41. **Two texture units on the chip** (found during device testing; `mbxtri-drm -N 5 -y B -z C`, `mbxtri -N 5 ...`: unit 0 and unit 1 sample the same
    8 x 8 linear texture, four texel pairs, 0 differing pixels in all of them). State word bits 16-18 = 5; the header mask names
    both units (bits 2 and 3) and sends A, B, C for each; the vertex program writes unit 0's coordinate to `o3.xy` and unit 1's to
    `o4.xy` (with `o4` unwritten unit 1 samples (0, 0): the picture is its texel at the corner). Unit 1's combiner `0x56883610` is
    colour and alpha A = previous, B = texel, C = 1 - 1: previous x texel, so unit 0 replace (`0xd6887610`) or modulate (`0x86884610`)
    followed by it is texel0 x texel1 (x the primary colour). Trilinear sweeps with two units (the second unit with bits 29 and
    30, `maxlev` 5, 11 or 15 in word A) still sample level 0.


42. **The offset colour** (found during device testing; `mbxtri-drm -O` with and without `-N`, `-Y`, `-y -z`, against `mbxtri -O`: six cases,
    0 differing pixels each). State word bit 23 with the vertex program writing `o2` (BGRA like `o1`; its alpha is the fog
    factor) adds `o2`'s red, green and blue to the texture units' result in 8 bits after the last unit, clamped, with or
    without a texture and with one or two units. `o1` and `o2` are clamped to 0..1 before the TSP, so the primary colour
    cannot scale a texel up and the offset colour cannot subtract. The driver uses it for the additive term of a fragment
    shader (`texel * a + b`): a shader is parsed into sums of products of texels, varyings, fragment uniforms and constants,
    the term with texels is the combiner (replace or modulate, one unit or two; colour and alpha have their own halves, so
    a shader may take only the alpha from a texture), the rest is `o2`, and the vertex program evaluates the coefficients
    (exact when they are affine in the varyings). An additive term in the alpha lane has no place on the chip (the
    shader takes the CPU).


43. **One-byte texels** (found during device testing; `mbxtri-drm -N 2 -V 5 -Q lo:hi [-H ...]`, five cases, 0 differing pixels): texel format 5
    returns the byte in red, green, blue **and alpha** (the replace combiner writes it to all four channels of the target),
    filters like the others, and a zero byte is an all-zero texel like black (a bilinear sample next to it is a step, README
    assumption 39). GL's L8 is (l, l, l, 1) and A8 is (0, 0, 0, a): the driver takes A8 for shaders that use only its alpha
    (the glyph atlas) and lets L8's alpha lane use the primary colour's alpha instead of the byte (a per-draw combiner word);
    an A8 texture in a shader's colour lanes, whose colour is zero, goes to the CPU. Zero bytes are stored as 1.
