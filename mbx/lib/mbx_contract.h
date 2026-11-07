/* SPDX-License-Identifier: MIT */
/*
 * What a client does before a scene for the chip to take it (mbx/README.md, assumption 31),
 * as one header: the free list's registers, the contexts, the background object, the region
 * array, the tail pointers and bit 30 before the render. All of it was found during device testing: what a client does before a scene, and what the
 * N810 did when these were missing (garbage, and writes outside the target).
 *
 * Portable C over two callbacks: device memory (as the MBX sees it, through its MMU) and
 * the registers. The kernel driver (mbx/drm/) and Mesa's shim implement the same steps
 * natively; keep the formulas the same.
 */
#ifndef MBX_CONTRACT_H
#define MBX_CONTRACT_H

#include <stdint.h>
#include <string.h>

struct mbx_io {
	void *opaque;
	void (*wr32)(void *opaque, uint32_t dev, uint32_t v);	/* device memory */
	uint32_t (*rd32)(void *opaque, uint32_t dev);
	void (*reg)(void *opaque, uint32_t reg, uint32_t v);	/* a register write */
};

#define MBX_TILE_PX		16
/* 0x85c's base bits, with the tile grid in bits 10-29 (the stock stack's; not measured on the device) */
#define MBX_TA_BASE		0xe1u
#define MBX_BG_WORDS		26
#define MBX_BG_RESERVED		0x210	/* bytes the chip reserves at the buffer's start (132 words) */

/* The macro tile's side in tiles, as the TA picks it (mbx/ta.c) */
static inline uint32_t mbx_mt_size(uint32_t tiles)
{
	return tiles <= 32 ? 4 : tiles <= 64 ? 8 : 16;
}

/* The region array's entries: the padded grid, in macro tiles */
static inline uint32_t mbx_region_entries(uint32_t tx, uint32_t ty)
{
	uint32_t mx = mbx_mt_size(tx), my = mbx_mt_size(ty);

	return ((tx + mx - 1) / mx * mx) * ((ty + my - 1) / my * my);
}

/*
 * The free list and the registers around it, before the build (0x838): the table, the
 * first page (the buffer's base page is the background object's), the last, the base
 * 0x83c, and the two that stock software writes with them. The caller writes 0x838
 * and waits for EVM_DALLOC.
 */
static inline void mbx_contract_freelist(const struct mbx_io *io, uint32_t table_va, uint32_t pb_va,
					 uint32_t last_page)
{
	io->reg(io->opaque, 0x824, table_va);
	io->reg(io->opaque, 0x828, (pb_va >> 12) + 1);
	io->reg(io->opaque, 0x82c, last_page);
	io->reg(io->opaque, 0x83c, pb_va);
	io->reg(io->opaque, 0x6d8, 0x09000000);
	io->reg(io->opaque, 0xce0, 1);
}

/* The parameter buffer's pages, filled with end words before the free list is built (list blocks end in one) */
static inline void mbx_contract_pb_fill(const struct mbx_io *io, uint32_t pb_va, uint32_t pages)
{
	uint32_t i;

	for (i = 0; i < pages * 1024; i++)
		io->wr32(io->opaque, pb_va + 4 * i, 0xf0000000u);
}

/* Two TA setup registers the stock stack writes (float 1e-5, the depth unit), before every TA start */
static inline void mbx_contract_ta_setup(const struct mbx_io *io)
{
	io->reg(io->opaque, 0x854, 0x3727c5ac);
	io->reg(io->opaque, 0x858, 0x3727c5ac);
}

/*
 * The TA and EVM contexts: zeroed, 16 KB apart and padded (the chip writes past their
 * nominal sizes: the TA's is a page, the EVM's 520 bytes), then loaded as the stock stack
 * loads them (0x810 = 0, 0x818 = 1: the context's scene to the TA context memory).
 * ctx_va: a 32 KB block, 16 KB aligned: the TA's context first, the EVM's 16 KB after it.
 */
static inline void mbx_contract_contexts(const struct mbx_io *io, uint32_t ctx_va)
{
	uint32_t i;

	for (i = 0; i < 0x8000; i += 4)
		io->wr32(io->opaque, ctx_va + i, 0);
	io->reg(io->opaque, 0x820, ctx_va);			/* TA context memory (the model's name) */
	io->reg(io->opaque, 0x864, ctx_va + 0x4000);		/* EVM context */
	io->reg(io->opaque, 0x810, 0);
	io->reg(io->opaque, 0x818, 1);
}

static inline uint32_t mbx_f2u(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

/*
 * The background object, the first 26 words of the parameter buffer: a triangle twice the
 * padded size, textured with the render target itself (a linear texture, 2048 x 2048 with
 * clamping, so any texel is in range), so a tile starts from the pixels the target holds.
 * stride_px: the target's row in pixels (a multiple of 16); fmt565: RGB565, else ARGB8888.
 */
static inline void mbx_contract_background(const struct mbx_io *io, uint32_t pb_va, uint32_t target_va,
					   uint32_t stride_px, uint32_t tiles_x, uint32_t tiles_y, int fmt565)
{
	uint32_t mx = mbx_mt_size(tiles_x), my = mbx_mt_size(tiles_y);
	uint32_t pad_w = (tiles_x + mx - 1) / mx * mx * MBX_TILE_PX;
	uint32_t pad_h = (tiles_y + my - 1) / my * my * MBX_TILE_PX;
	uint32_t o[MBX_BG_WORDS] = {
		0xe0000000, 0xa8800000, 0x0e000000, 0xd6887610, 0x22220e80,
		0, 0, 0, 0, 0, 0,
		0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
		0, 0, 0, 0, 0, 0, 0, 0, 0,
	};
	uint32_t i;

	o[1] |= fmt565 ? 0x4000 : 0x18000;			/* the texel format */
	o[2] |= (stride_px / 16) << 18 | target_va >> 7;	/* the stride in sixteenths of a pixel, the address / 128 */
	o[7] = mbx_f2u((float)(2 * pad_w));			/* the far corners */
	o[10] = mbx_f2u((float)(2 * pad_h));
	o[21] = mbx_f2u((float)pad_w / 1024.0f);		/* the texture coordinates, texel for pixel */
	o[25] = mbx_f2u((float)pad_h / 1024.0f);
	for (i = 0; i < MBX_BG_WORDS; i++)
		io->wr32(io->opaque, pb_va + 4 * i, o[i]);
}

/*
 * The background object for a frame that starts with a clear (README assumption 44): the same object, but its texture is 16 x 8
 * texels of the clear colour, written in the buffer's first page past the object (offset 0x400, 128 aligned), read
 * 8 x 8 with clamping, every texture coordinate zero, so the background pass paints the colour and reads nothing of the
 * target. colour: ARGB8888, or RGB565 in the low 16 bits.
 */
#define MBX_FLAT_BG_OFFSET	0x400
static inline void mbx_contract_background_flat(const struct mbx_io *io, uint32_t pb_va, uint32_t tiles_x, uint32_t tiles_y,
						int fmt565, uint32_t colour)
{
	uint32_t i;

	mbx_contract_background(io, pb_va, pb_va + MBX_FLAT_BG_OFFSET, 16, tiles_x, tiles_y, fmt565);
	io->wr32(io->opaque, pb_va + 4 * 1, 0xa0000000 | (fmt565 ? 0x4000 : 0x18000));
	io->wr32(io->opaque, pb_va + 4 * 21, 0);
	io->wr32(io->opaque, pb_va + 4 * 25, 0);
	for (i = 0; i < 16 * 8; i++)
		io->wr32(io->opaque, pb_va + MBX_FLAT_BG_OFFSET + 4 * i, fmt565 ? (colour & 0xffff) * 0x10001u : colour);
}

/*
 * The region array, before the TA: every entry 0x22000000 | its macro tile's origin (y << 8
 * | x, in tiles), word 1 clear. The TA clears bit 25 and writes the list address for the tiles
 * that get a list.
 */
static inline void mbx_contract_regions(const struct mbx_io *io, uint32_t regions_va, uint32_t tx, uint32_t ty)
{
	uint32_t mx = mbx_mt_size(tx), my = mbx_mt_size(ty);
	uint32_t nx = (tx + mx - 1) / mx, ny = (ty + my - 1) / my, i, j, n, k = 0;

	for (j = 0; j < ny; j++)
		for (i = 0; i < nx; i++)
			for (n = 0; n < mx * my; n++, k++) {
				io->wr32(io->opaque, regions_va + 8 * k, 0x22000000 | (j * my) << 8 | i * mx);
				io->wr32(io->opaque, regions_va + 8 * k + 4, 0);
			}
}

/*
 * The render registers the model did not know the values of (found during device testing; most of
 * them are not understood, "what GLES writes"), queued with the rest before the kick: 0x860 is the tile
 * grid, (tiles_y - 1) << 16 | (tiles_x - 1), with the macro tile's width in bits 4-6 of X (0 up to 32 tiles).
 */
static inline void mbx_contract_render_regs(const struct mbx_io *io, uint32_t tx, uint32_t ty)
{
	io->reg(io->opaque, 0x60c, 0);
	io->reg(io->opaque, 0x614, 0x1e3ce508);
	io->reg(io->opaque, 0x618, 0x1e3ce508);
	io->reg(io->opaque, 0x644, 0);
	io->reg(io->opaque, 0x648, 1);
	io->reg(io->opaque, 0x64c, 0);
	io->reg(io->opaque, 0x6d0, 1);
	io->reg(io->opaque, 0x860, (ty - 1) << 16 | (tx - 1));
}

/* The tail pointers, zeroed before each scene (the address in 0x840 must be 64 KB aligned) */
static inline void mbx_contract_tails(const struct mbx_io *io, uint32_t tails_va, uint32_t bytes)
{
	uint32_t i;

	for (i = 0; i < bytes; i += 4)
		io->wr32(io->opaque, tails_va + i, 0);
}

/* After the TA, before the render: bit 30 on every region entry (the stock daemon does it) */
static inline void mbx_contract_prerender(const struct mbx_io *io, uint32_t regions_va, uint32_t tx, uint32_t ty)
{
	uint32_t n = mbx_region_entries(tx, ty), k;

	for (k = 0; k < n; k++)
		io->wr32(io->opaque, regions_va + 8 * k, io->rd32(io->opaque, regions_va + 8 * k) | 0x40000000);
}

#endif
