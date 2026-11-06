// SPDX-License-Identifier: MIT
/*
 * mbxemu: the 2D core (the words host software writes to the 2D
 * slave port). A blit runs when its destination
 * rectangle arrives; 0xf0000000 raises 2DSYNC (INT_STATUS bit 10).
 *
 *   a  destination: format | stride (bytes), then the address
 *   9  source: format | stride, then the address
 *   3  source origin: x << 14 | y
 *   6  scale: source per destination pixel, 5.5 fixed point
 *   2  colour key (0x20000000 destination, 1 source): key, mask; or
 *      alpha (0x20000004): 0x0d500000 | a << 12 global, 0x09100000 source alpha over
 *   0  1-4 clip rectangles: left | right << 16, top | bottom << 16 each
 *   b  pattern surface (parsed; drawn as the solid colour)
 *   e  1-bpp mask surface (parsed; ignored)
 *   8  the blit: bit 16 no solid colour, 17 alpha, 18 clip, 20 source key,
 *      23/24 copy direction; ROP3 in bits 0-7; [the colour], then the
 *      destination rectangle x0 << 16 | y0, x1 << 16 | y1 (ends exclusive)
 *   7  no-op
 *   f  sync
 */
#include <stdlib.h>
#include <string.h>

#include "emu.h"

enum { W_NONE, W_DST, W_SRC, W_KEY, W_ALPHA, W_CLIP, W_PATTERN, W_MASK, W_BLIT };

void twod_reset(struct mbxemu *e)
{
	memset(&e->twod, 0, sizeof(e->twod));
	e->twod.xs = e->twod.ys = 32;
}

static int fmt_bpp(uint32_t fmt)
{
	switch (fmt) {
	case 0x60000: return 4;
	case 0x58000: return 3;
	case 0x50000: case 0x48000: case 0x38000: return 2;
	default: return 0;	/* 1 bpp and the rest: not modelled */
	}
}

static uint32_t to_argb(uint32_t fmt, uint32_t v)
{
	uint32_t r, g, b, a;

	switch (fmt) {
	case 0x60000:
		return v;
	case 0x58000:
		return 0xff000000 | (v & 0xffffff);
	case 0x50000:
		r = v >> 11 & 31; g = v >> 5 & 63; b = v & 31;
		return 0xff000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
	case 0x48000:
		r = v >> 10 & 31; g = v >> 5 & 31; b = v & 31;
		return (v & 0x8000 ? 0xff000000 : 0) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 |
		       (b << 3 | b >> 2);
	case 0x38000:
		a = v >> 12 & 15; r = v >> 8 & 15; g = v >> 4 & 15; b = v & 15;
		return a * 0x11u << 24 | r * 0x11u << 16 | g * 0x11u << 8 | b * 0x11u;
	}
	return 0;
}

static uint32_t from_argb(uint32_t fmt, uint32_t c)
{
	uint32_t a = c >> 24, r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;

	switch (fmt) {
	case 0x60000: return c;
	case 0x58000: return c & 0xffffff;
	case 0x50000: return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
	case 0x48000: return (a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
	case 0x38000: return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
	}
	return 0;
}

static uint32_t rd_px(struct mbxemu *e, uint32_t addr, int bpp)
{
	uint32_t v = 0;

	dev_read(e, addr, &v, (uint32_t)bpp);
	return v;
}

/* ROP3 on bits: index P << 2 | S << 1 | D (0xcc copies the source, 0xf0
 * the pattern, 0x55 inverts the destination) */
static uint32_t rop3(uint32_t rop, uint32_t p, uint32_t s, uint32_t d)
{
	uint32_t r = 0;
	int k;

	for (k = 0; k < 8; k++)
		if (rop >> k & 1)
			r |= (k & 4 ? p : ~p) & (k & 2 ? s : ~s) & (k & 1 ? d : ~d);
	return r;
}

static uint32_t mix(uint32_t s, uint32_t d, uint32_t a)
{
	uint32_t r = 0;
	int k;

	for (k = 0; k < 32; k += 8) {
		uint32_t cs = s >> k & 255, cd = d >> k & 255;

		r |= ((cs * a + cd * (255 - a) + 127) / 255) << k;
	}
	return r;
}

static void blit(struct mbxemu *e)
{
	struct twod *t = &e->twod;
	uint32_t rop = t->blit & 0xff, x0 = t->buf[0] >> 16 & 0x1fff, y0 = t->buf[0] & 0x1fff;
	uint32_t x1 = t->buf[1] >> 16 & 0x1fff, y1 = t->buf[1] & 0x1fff, x, y, w, h, *src = NULL;
	int dbpp = fmt_bpp(t->dfmt), sbpp = fmt_bpp(t->sfmt);
	int uses_src = ((rop ^ rop >> 2) & 0x33) != 0;

	e->stats.blits++;
	if (!dbpp || x1 <= x0 || y1 <= y0) {
		if (!dbpp)
			emu_log(e, "2D: destination format %#x not modelled", t->dfmt);
		return;
	}
	w = x1 - x0;
	h = y1 - y0;
	pcache_begin(e);
	if (uses_src) {
		if (!t->have_src || !sbpp) {
			emu_log(e, "2D: a ROP that reads the source, no source (format %#x)", t->sfmt);
			pcache_end(e);
			return;
		}
		/* the source read first, whole: overlapping copies come out as
		 * the direction bits ask, whichever they are */
		if (!(src = malloc((size_t)w * h * 4))) {
			pcache_end(e);
			return;
		}
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				uint32_t sx = t->sx + x * t->xs / 32, sy = t->sy + y * t->ys / 32;

				src[y * w + x] = to_argb(t->sfmt, rd_px(e, t->saddr + sy * t->sstride + sx * sbpp,
									sbpp));
			}
	}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint32_t dx = x0 + x, dy = y0 + y, da = t->daddr + dy * t->dstride + dx * dbpp;
			uint32_t d = rd_px(e, da, dbpp), s = src ? from_argb(t->dfmt, src[y * w + x]) : 0;
			uint32_t r, sargb = src ? src[y * w + x] : 0;
			int k, in = !(t->blit & 0x40000);

			for (k = 0; k < t->nclip && !in; k++)
				in = dx >= (t->clip[k][0] & 0xffff) && dx < t->clip[k][0] >> 16 &&
				     dy >= (t->clip[k][1] & 0xffff) && dy < t->clip[k][1] >> 16;
			if (!in)
				continue;
			/* colour key (assumed): a source key skips source
			 * pixels that match, a destination key writes only over ones */
			if (t->key_on) {
				int src_key = t->key_src || (t->blit & 0x100000);
				uint32_t kv = src_key ? sargb : to_argb(t->dfmt, d);

				if (((kv & t->key_mask) == (t->key & t->key_mask)) == src_key)
					continue;
			}
			r = rop3(rop, t->colour, s, d);
			if (t->alpha_on && (t->blit & 0x20000)) {
				uint32_t a = t->alpha_pp ? sargb >> 24 : t->alpha;

				r = from_argb(t->dfmt, mix(to_argb(t->dfmt, r), to_argb(t->dfmt, d), a));
			}
			dev_write(e, da, &r, (uint32_t)dbpp);
		}
	free(src);
	pcache_end(e);
	emu_trace(e, "2D: blit rop %02x, %ux%u at %u,%u of %#x", rop, w, h, x0, y0, t->daddr);
}

/* The words a command wants after its first, and what to do with them */
static void done(struct mbxemu *e)
{
	struct twod *t = &e->twod;
	int k;

	switch (t->state) {
	case W_DST:
		t->daddr = t->buf[0] & 0x3fffffc;
		break;
	case W_SRC:
		t->saddr = t->buf[0] & 0x3fffffc;
		t->have_src = 1;
		break;
	case W_KEY:
		t->key_on = 1;
		t->key = t->buf[0];
		t->key_mask = t->buf[1];
		break;
	case W_ALPHA:
		t->alpha_on = 1;
		t->alpha_pp = (t->buf[0] & 0xfff00000) == 0x09100000;
		t->alpha = t->buf[0] >> 12 & 0xff;
		break;
	case W_CLIP:
		for (k = 0; k < t->nclip; k++) {
			t->clip[k][0] = t->buf[2 * k];
			t->clip[k][1] = t->buf[2 * k + 1];
		}
		break;
	case W_BLIT:
		if (!(t->blit & 0x10000)) {
			t->colour = t->buf[0];
			memmove(t->buf, t->buf + 1, 2 * sizeof(t->buf[0]));
		}
		blit(e);
		/* what a blit set up goes with it; the destination stays */
		t->have_src = 0;
		t->key_on = t->alpha_on = 0;
		t->nclip = 0;
		t->xs = t->ys = 32;
		break;
	}
	t->state = W_NONE;
	t->want = t->got = 0;
}

void twod_word(struct mbxemu *e, uint32_t v)
{
	struct twod *t = &e->twod;

	if (e->trace > 1)
		emu_log(e, "2D: %08x", v);
	if (t->want) {
		t->buf[t->got++] = v;
		if (t->got == t->want)
			done(e);
		return;
	}
	switch (v >> 28) {
	case 0x0:
		if (v >= 1 && v <= 4) {
			t->nclip = (int)v;
			t->state = W_CLIP;
			t->want = 2 * (int)v;
			return;
		}
		break;
	case 0x2:
		t->state = (v & 0xf) == 4 ? W_ALPHA : W_KEY;
		t->key_src = v & 1;
		t->want = t->state == W_ALPHA ? 1 : 2;
		return;
	case 0x3:
		t->sx = v >> 14 & 0x1fff;
		t->sy = v & 0x1fff;
		return;
	case 0x6:
		t->xs = v >> 18 & 0x3ff;
		t->ys = v >> 4 & 0x3ff;
		return;
	case 0x7:
		return;
	case 0x8:
		t->blit = v;
		t->state = W_BLIT;
		t->want = v & 0x10000 ? 2 : 3;
		return;
	case 0x9:
		t->sfmt = v & 0x78000;
		t->sstride = v & 0x7fff;
		t->state = W_SRC;
		t->want = 1;
		return;
	case 0xa:
		t->dfmt = v & 0x78000;
		t->dstride = v & 0x7fff;
		t->state = W_DST;
		t->want = 1;
		return;
	case 0xb:
		t->state = W_PATTERN;
		t->want = 2;
		emu_log(e, "2D: a pattern surface (not modelled: the solid colour stands in)");
		return;
	case 0xe:
		t->state = W_MASK;
		t->want = 3;
		emu_log(e, "2D: a mask surface (not modelled)");
		return;
	case 0xf:
		if (v == 0xf0000000) {
			emu_raise(e, I_2D);
			return;
		}
		break;
	}
	emu_log(e, "2D: unknown word %08x", v);
}
