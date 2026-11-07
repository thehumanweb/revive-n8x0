# GLES2 coverage on the MBX1: what the hardware offers, what the driver uses

Measured with piglit's GLES2 tests on the host sim (`mesa/scripts/run-piglit.sh`, 141 tests; the list is
`mesa/tests/piglit-gles2.txt`, the last result `mesa/tests/piglit-baseline.txt`): at the start 103 pass, 6 fail, 1 crash,
31 skip. The driver's own feature tests (`mesa/tests/gles2_feat.c`, 31 probes: primitives, scissor, textures, fragment
uniforms, blending, culling, depth, and the CPU fallback's cases) pass on the host sim **and on the N810** (Mesa on the real
chip, softpipe on its ARM core, 2026-11-07). Piglit's GLES2 set on the sim: 117 pass, 0 fail, 0 crash, 31 skips (optional extensions: framebuffer fetch, blend_func_extended, s3tc, ES3 contexts; the `fragdepth` crash was piglit's window event loop without `PIGLIT_NO_WINDOW=1`, now set by `run-piglit.sh`). Most passes are compile-only GLSL ES tests; almost no test that draws and reads back passes yet, because
the driver draws only untextured, unscissored, fixed-colour-pipe triangles.

## Baseline failures and skips (2026-11-06)

| Test | Why | What fixes it |
|---|---|---|
| `glsl-fs-pointcoord`, `glsl-no-vertex-attribs` | `mbx: primitive mode 0 (points) reached the driver` | points: the TA's primitive types |
| `execution@sanity`, `unroll-do-while-false-loop-only-once` | `no program for this vertex and fragment shader pair` | fragment shaders that do not reduce to the fixed pipe: a fallback |
| `minmax_gles2` | `GL_MAX_TEXTURE_IMAGE_UNITS` is 2 (ES 2.0 needs 8) | honest limits, multipass or fallback for more than 2 units |
| `built-in constants` | `gl_MaxVertexTextureImageUnits` is 0 and the test declares an array of that size | none (a test limit; ES allows 0) |
| `fragdepth_gles2` | crash on an incomplete framebuffer | find the crash |
| 8 + 8 skips | `EXT_shader_framebuffer_fetch(_non_coherent)` | not exposed (the background object is the nearest thing the chip has) |
| 3 + 3 skips | `EXT_blend_func_extended`, `OES_texture_float` | not exposed |

## The chip's capabilities (the model: `mbx/README.md`, `render.c`, `texture.c`) and the driver today

| GLES2 feature | MBX mechanism | Driver |
|---|---|---|
| Triangles, strips, fans | TA primitive types | done |
| Points, lines (width, `gl_PointSize`) | TA primitive types (`0xe`, `0x6`), one size or width per object | done: points (size from a constant or uniform `gl_PointSize`), lines, strips, restarts; `gles2_feat` probes them |
| Vertex shaders (VGP, 256 instructions) | VGP programs | done for the NIR subset in `mbx_compile.c` |
| Depth test and write, all 8 functions | ISP, state word bits 9-11, 7 | done |
| Culling, polygon offset | TA cull bits, state bit 23 | cull done, offset not |
| Blending: any (src, dst) factor pair, saturate | state word factors, bit 13 | done for add; constant colour, separate alpha, subtract not |
| Colour mask | state bit 8 (none), no per-channel mask | all-or-nothing only |
| Alpha test (not GLES2, but the chip has it) | state bit 14, alpha word | used for `discard` of constant alpha only (planned) |
| Scissor | four user clip planes (o5.xyzw: a dot product of the clip position with a constant built from the viewport and the rectangle), enabled in the sections word; the colour clear is a CPU fill of the rectangle | done on the sim: draws pixel-exact (`scissor_draw`), clears (`scissor_clear`); points and lines straddling the edge are dropped whole, not clipped; a scissored depth clear is not done |
| Textures: 2D, NPOT, RGB565/ARGB1555/4444/8888/PVRTC, linear or twiddled, mipmaps (twiddled), nearest/bilinear/trilinear | two texture units per pass, texture words A/B/C | done on the sim: one unit, linear power-of-two storage (the chip reads linear textures with a stride, as the background object does), NPOT by coordinate scaling in the VGP and edge repeat in the padding, nearest/bilinear, clamp/repeat, formats ARGB8888/565/4444/1555/LA88, `replace` and `modulate` fragment shaders (`gles2_feat` tex_* tests). Two units: `texture2D(a, uv) * texture2D(b, uv2) [* colour]` runs on the chip (unit 1 = previous x texel, README assumption 41). One-byte textures (A8, L8) are supported (README assumption 43). Trilinear: one unit (README assumption 47). Not done: PVRTC. On the chip: a textured triangle through `mbxtri-drm -N 2` equals the model's picture exactly (replace, modulate, bilinear); one unit is state bits 16-18 = 2 (README assumption 38); the Mesa path on the unit follows the next device image |
| Wrap modes | texture word flags (clamp, wrap; mirror?) | not done |
| Cube maps | none in hardware (not found; not looked for) | six linear faces per level, sampled by softpipe (`cube` test); no cube render targets. 3D and vertex textures: none |
| Fragment shaders | fixed combiners (modulate, add, decal, ...) per texture unit, offset colour, fog | on the chip: sums of products of up to two texels, varyings, uniforms and constants (texel x colour + offset colour, `mix` with a constant weight, texture alpha only, `a * b + c`), colours affine in the varyings, and, in **grid mode** (README assumption 48), smooth colours that are not (`normalize`, `dot`, `pow`, `max`, `min`, `fract`, `rcp`, `rsq` and the like on varyings and uniforms, times at most one texel per lane): the vertex program evaluates them at the corners of an 8-pixel grid and the chip interpolates; everything else (a texel squared, texel + texel, a texel inside a nonlinear function, an additive alpha, a texel scaled above 1, `discard` other than on alpha) through the CPU fallback |
| Fog | state bit 24, fog table | not exposed (not GLES2) |
| Stencil | none (depth only) | none: report no stencil bits |
| Framebuffer objects, RGB565/RGBA4/5551/8888, depth 16/24 | render target format register `0x650`, the external Z buffer | window surfaces and simple FBOs only |
| Clear (colour, depth) | background object, depth-only clear object | colour+depth clear done, scissor not |
| `glReadPixels`, `glCopyTexImage`, blits | CPU or the 2D core (`twod.c`: ROPs, blits) | readpixels and plain copies done; the rest not |
| Compressed textures (PVRTC) | native texel format | not exposed |

A full-target colour clear is not done on the CPU: the driver keeps it pending and the next job's background pass paints it (README assumption 44); a CPU draw, blit, read-back, sampler or other target settles it with a CPU fill. The other rows below were measured before this and still include the CPU clear (about 7.5 ms at 800x480, 2 ms at 400x240).

## Performance (N810, offscreen FBO, 2026-11-07; `mesa/tests/gles2_fps.c`: clear, draw, `glFinish` per frame)

| Scene | 200x120 | 400x240 | 800x480 |
|---|---|---|---|
| plain full-screen quad (chip) | 2.4 ms (411 fps) | 3.8 ms (261 fps) | 9.1 ms (110 fps) |
| the same with the clear on the CPU (`MBX_CPU_CLEAR=1`; before the flat background) | 2.9 ms (340 fps) | 5.7 ms (176 fps) | 16.6 ms (60 fps) |
| one small triangle (chip) | | 6.7 ms (149 fps) | 20.2 ms (50 fps) |
| textured quad, replace / modulate (chip, 256x256 bilinear) | | 9.1 / 9.2 ms (109 fps) | 25.2 / 25.0 ms (40 fps) |
| 400 small triangles (chip) | | 13.1 ms (76 fps) | 28.6 ms (35 fps) |
| full-screen quad, shader on the CPU (softpipe; `discard` on a colour lane) | 163 ms (6 fps) | 677 ms (1.5 fps) | 2539 ms (0.4 fps) |
| full-screen quad, non-affine shader in grid mode (a varying squared) | | 9.1 ms (110 fps) | 14.6 ms (68 fps) |

The chip's frame time follows the number of 16 x 16 tiles with a list (about 11-15 microseconds a tile plus a fixed 2-3 ms): the
background pass reads and writes every such tile, and the colour clear is a CPU fill of the whole target first. The CPU
fallback costs about 25-40 times the chip's time at every size (roughly 4.5 microseconds a pixel): usable for small areas,
not for full-screen shaders at interactive rates.

## The CPU fallback (`mbx_fallback.c`)

What the chip cannot do is drawn by Mesa's softpipe into the same buffers, so every GLES2 program runs: a fragment shader that is
not a product of a texel, a varying, a uniform and constants (any arithmetic, `discard`, `gl_FragDepthEXT`, two or more
textures, `gl_PointCoord`), a partial colour mask, separate / subtract / constant-colour blending, polygon offset, stencil, a draw
without vertex attributes. Blits that are not plain copies (`glGenerateMipmap`, scaling, format conversion) go the same way (softpipe's blit, read back into the MBX's memory for a texture only softpipe holds). The pending MBX job finishes first (its render is waited for), softpipe draws into the render
target's memory through a small software winsys (a display target is an MBX buffer, mapped), and later MBX jobs start from
what it left (the background object is the target). Textures are copied to softpipe when the CPU wrote them (render
targets are shared in place); vertex, index and constant buffers are read through user pointers; the shaders are softpipe's
own (a copy of the NIR from before our lowering). Limits: the MBX keeps depth on chip and never in memory, so depth does not
carry between MBX and CPU draws of one frame (a frame of either kind is right); mipmaps of a render target are not shared; mipmapped sampling is the CPU's only (the chip reads linear textures, level 0 only: on-chip mipmaps need twiddled storage); it
is slow (softpipe on the 330 MHz ARM1136), a path for correctness.

## Mipmaps

Power-of-two textures of at least 8 x 8 with more than one level are stored twiddled (the chip's chain, no padding; the CPU
sees a staging copy in linear order), and the sampler's mipmap filter sets word B bit 29 and level bias 9 (nearest level; the
`_MIPMAP_LINEAR` filters get the nearest level too: a single-unit draw with a `LINEAR` mip filter uses the chip's trilinear mode, README assumption 47; two units still take the nearest level).
`glGenerateMipmap` is a blit on the CPU (softpipe). The chip cannot render into a twiddled texture: such draws and clears go to
the CPU fallback. Non-power-of-two and render-to-texture-only textures stay linear with the base level only.

## Hardware limits found on the unit (texturing)

- Bilinear filtering is skipped next to a texel whose R, G and B are all zero: a black-to-light edge is a step. The driver writes
  a black texel as colour 1 on upload for ARGB8888 and LA88 (1/255 off; `bilinear_black` tests it); the 16-bit formats keep
  their zeros (a nudge would be 3-6 per cent), and texels the chip renders itself cannot be nudged.
- 16-bit texel colour fields are shifted, not replicated (white 565 is 248, 252, 248).
- One texture unit is state bits 16-18 = 2; textures are linear power-of-two with a stride (README assumptions 38 and 39).

## Order of work (each step is measured by the piglit list above, then by dEQP-GLES2 once it is built)

1. **Cheap correctness:** points and lines, scissor (render clip rectangles), honest `MAX_*` limits, the `fragdepth` crash, the colour
   mask for the cases the chip can do, constant-colour blending where it reduces to the factor pairs.
2. **Textures:** 2D textures (formats, filters, wraps, mipmaps with the twiddle upload), the two texture units as combiners,
   `glTexImage2D`/`SubImage`/`CopyTexImage`, `generateMipmap`, PVRTC upload; this is what most GLES2 programs need first.
3. **Fragment shaders:** map GLSL to the combiners (texture x vertex colour, constant colour, simple arithmetic); the shaders
   that do not map need a decision: a CPU fallback (the render core's arithmetic is in `mbx/render.c`), more passes, or
   refusing them at link time with an error (Mesa's GLES2 allows a link failure for an unsupported program).
4. **Wider test coverage:** dEQP-GLES2 (piglit's `deqp_gles2` profile needs the dEQP binaries: a large build), and our own
   draw-and-probe tests in `mesa/tests/` for each feature above, with the model's picture as the expectation.

## Added 2026-11-07 (after the table above)

- Cube maps: CPU-sampled (six linear faces per level), no cube render targets.
- `gl_PointSize` from a vertex attribute: one object per point (`point_sizes`).
- `if (alpha < k) discard;` (and mirrors): the chip's alpha function with state bit 14 (README assumption 45; `alpha_discard`).
- A full-target colour clear costs nothing (README assumption 44).
- A textured first job in a new process used to read the previous process's texels (stale TLB); fixed by loading the TLB at every address-space switch (README assumption 46), so the flat clear works with textured scenes too.
- A lit texture (a texel times a per-pixel `normalize`/`dot` term) runs on the chip in grid mode: the texture unit takes the texel, the vertex program the lighting at grid corners, the combiner multiplies them (`grid_tex`; no new code was needed beyond grid mode).
- Zero colour mask is on the chip; a partial mask, stencil (works now, on the CPU: README assumption 49), `discard` on a colour lane and constant-colour blend are still softpipe (measured reasons: no per-channel write mask, no stencil, the alpha function tests only alpha, no constant factor code).
- **Depth carries between chip and CPU draws** (external Z, README assumption 7): a job stores its depth to memory and loads it back (`MBX_SUBMIT_ZLOAD`, `0x610` bits 0 and 1, region bit 29) when a CPU draw, a blit or a framebuffer switch follows, or when the memory already holds current depth; softpipe's depth twin wraps the same memory (`Z32_FLOAT`). Tests `depth_cpu_cpu`, `depth_chip_cpu`, `depth_chip_cpu_chip`. An ordinary frame (depth clear, draws, swap) pays nothing: a depth-tested full-screen quad is 3.8 ms at 400x240 and 9.1 ms at 800x480 with or without it; storing after every depth-using job would cost 1.2 and 4.6 ms. Not carried: a flush forced by a read-back or a texture use in the middle of a frame (the depth is lost as before), partial depth clears, depth textures.
- **Stencil works, on the CPU** (README assumption 49): `GL_DEPTH24_STENCIL8_OES` and `GL_STENCIL_INDEX8` framebuffers are complete, every stencil-tested draw is softpipe's (about 5 microseconds a pixel), with `stencil` and `stencil_only` tests (clip shape with REPLACE then a draw where it equals 1, INCR counting). Before this the driver advertised no stencil at all (no EGL config with stencil bits, incomplete FBOs). Depth does not carry between chip and CPU draws in a stencil framebuffer. The fast version (depth tricks the stock stack uses for scissor and path coverage) is open.
- **Partial renders** (README assumption 50): `pipe_screen::set_damage_region` is implemented, so `EGL_KHR_partial_update` is advertised (with `EGL_EXT_buffer_age` on the GBM platform); only the damaged tiles are rendered, the rest of the buffer keeps its pixels. Test `damage` (through the `MBX_TEST_DAMAGE` hook, since a window surface is needed for the EGL calls); 800 x 480 textured quad 17.6 ms full, 5.5 ms for a sixteenth. `eglSwapBuffersWithDamage` for the panel update is the kernel/omapdrm side (`PARTIAL_UPDATES.md`, user-pointed) and the compositor or SDL's; the driver's per-frame diff (`mbx_frame_damage`) is not built, the window system's damage is used instead. The tile at the end of the region array is always rendered.

