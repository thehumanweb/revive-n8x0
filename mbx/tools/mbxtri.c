// SPDX-License-Identifier: MIT
/*
 * mbxtri: a Gouraud triangle through the MBX model on the host, no kernel.
 * It does what a driver does for one frame: the TA's setup registers, the
 * stream (object header, a VGP program and its constants, the vertices), the
 * end word, then the render registers and the kick. Every word below is one
 * mbx/ta.c, core.c and render.c read; see mbx/README.md for what is assumed.
 *
 *   mbxtri [-a degrees] [-v] [-o out.ppm]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../mbxemu.h"

#define RAM_BASE	0x80000000u
#define RAM_SIZE	(8u << 20)
#include "tri_frame.h"

#define W		TRI_W
#define H		TRI_H

/* registers (mbx/emu.h) */
#define R_INT_ENABLE	0x130
#define R_INT_CLEAR	0x134
#define R_INT_STATUS	0x12c
#define R_REGIONS	0x608
#define R_RENDER_CTL	0x610
#define R_SAMPLING	0x61c
#define R_FOG_COLOUR	0x63c
#define R_FORMAT	0x650
#define R_WIDTH		0x654
#define R_HEIGHT	0x658
#define R_ADDR		0x65c
#define R_STRIDE	0x660
#define R_KICK		0x680
#define R_HW_CONTEXT	0x6ec
#define R_TA_START	0x800
#define R_TA_CONTEXT_ID	0x810
#define R_PB_BASE	0x83c
#define R_TA_TAILS	0x840
#define R_TA_REGIONS	0x844
#define R_TA_WIDTH	0x84c
#define R_TA_HEIGHT	0x850
#define R_TA_CONFIG	0x85c

#define I_RENDER	0x4
#define I_TA_DONE	0x10
#define I_MASTER	0x8000

static uint8_t *ram;

static void mem_read(void *o, uint32_t phys, void *buf, uint32_t len)
{
	if (phys >= RAM_BASE && phys - RAM_BASE + len <= RAM_SIZE)
		memcpy(buf, ram + (phys - RAM_BASE), len);
	else
		memset(buf, 0, len);
}

static void mem_write(void *o, uint32_t phys, const void *buf, uint32_t len)
{
	if (phys >= RAM_BASE && phys - RAM_BASE + len <= RAM_SIZE)
		memcpy(ram + (phys - RAM_BASE), buf, len);
}

static void irq(void *o, int level) { (void)o; (void)level; }

static void logf_(void *o, const char *line) { (void)o; fprintf(stderr, "emu: %s\n", line); }

static struct mbxemu *E;

static void wr(uint32_t reg, uint32_t v) { mbxemu_reg_write(E, reg, v); }
static uint32_t rd(uint32_t reg) { return mbxemu_reg_read(E, reg); }
static void ta(uint32_t v) { mbxemu_ta_write(E, 0, v); }

int main(int argc, char **argv)
{
	struct mbxemu_host host = { .mem_read = mem_read, .mem_write = mem_write, .irq = irq };
	const char *out = "mbxtri.ppm";
	float deg = 0;
	int o, verbose = 0;
	uint32_t surf = RAM_BASE + 0x10000, regions = RAM_BASE + 0x100000, tails = RAM_BASE + 0x110000;
	uint32_t pb = RAM_BASE + 0x120000;
	uint32_t stream[128], words, status;
	int i, x, y;

	while ((o = getopt(argc, argv, "a:vo:")) != -1) {
		if (o == 'a') deg = (float)atof(optarg);
		else if (o == 'v') verbose = 1;
		else if (o == 'o') out = optarg;
		else return 2;
	}
	if (!(ram = calloc(1, RAM_SIZE)))
		return 1;
	host.log = logf_;
	if (!(E = mbxemu_new(&host)))
		return 1;
	if (verbose)
		mbxemu_set_trace(E, 1);

	/* the colour buffer: cleared by the CPU (no background object is set up) */
	for (i = 0; i < W * H; i++)
		((uint32_t *)(ram + (surf - RAM_BASE)))[i] = TRI_CLEAR;

	/* 1. the TA: tile grid (16-pixel tiles), regions, size, then start */
	wr(R_INT_ENABLE, I_MASTER | I_RENDER | I_TA_DONE);
	wr(R_INT_CLEAR, 0x7fff);
	wr(R_TA_CONFIG, 0x80 | (uint32_t)(W / 16 - 1) << 20 | (uint32_t)(H / 16 - 1) << 10);
	wr(R_TA_WIDTH, (uint32_t)W << 16);
	wr(R_TA_HEIGHT, (uint32_t)H << 16);
	wr(R_TA_REGIONS, regions);
	wr(R_TA_TAILS, tails);
	wr(R_TA_CONTEXT_ID, 0);
	wr(R_TA_START, 1);

	/* 2. the stream: object header, VGP program and constants, the triangle, the end word */
	words = tri_stream(stream, deg);
	for (i = 0; i < (int)words;) {
		uint32_t type = stream[i] >> 28, count = stream[i] & 0xffff, k;

		for (k = 0; k < count; k++) {
			if (type == CH_CTL)
				mbxemu_ta_ctl_write(E, stream[i + 1 + k]);
			else
				ta(stream[i + 1 + k]);
		}
		i += 1 + count;
	}

	status = rd(R_INT_STATUS);
	if (!(status & I_TA_DONE)) {
		fprintf(stderr, "mbxtri: no TA_COMPLETE (status %#x)\n", status);
		return 1;
	}

	/* 5. the render */
	wr(R_REGIONS, regions);
	wr(R_RENDER_CTL, 0x30000);	/* depth on chip: no external Z */
	wr(R_SAMPLING, 0x00020007);
	wr(R_FOG_COLOUR, 0);
	wr(R_FORMAT, 0xe);		/* ARGB8888 */
	wr(R_WIDTH, (uint32_t)(W - 1) << 16);
	wr(R_HEIGHT, (uint32_t)(H - 1) << 16);
	wr(R_ADDR, surf);
	wr(R_STRIDE, W);		/* in 32-bit words */
	wr(R_PB_BASE, pb);		/* its background object: zero, none */
	wr(R_HW_CONTEXT, 0);
	wr(R_KICK, 1);

	status = rd(R_INT_STATUS);
	if (!(status & I_RENDER)) {
		fprintf(stderr, "mbxtri: no RENDER_COMPLETE (status %#x)\n", status);
		return 1;
	}
	{
		struct mbxemu_stats st;

		mbxemu_stats(E, &st);
		fprintf(stderr, "mbxtri: %u scenes, %u prims, %u renders, %u tiles, %u stream errors\n",
			st.scenes, st.prims, st.renders, st.tiles, st.stream_errors);
	}

	{
		FILE *f = fopen(out, "wb");
		const uint32_t *p = (const uint32_t *)(ram + (surf - RAM_BASE));
		int lit = 0;

		if (!f)
			return 1;
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				uint32_t px = p[y * W + x];

				fputc(px >> 16 & 255, f);
				fputc(px >> 8 & 255, f);
				fputc(px & 255, f);
				lit += px != TRI_CLEAR;
			}
		fclose(f);
		fprintf(stderr, "mbxtri: %d of %d pixels drawn, wrote %s\n", lit, W * H, out);
		return lit ? 0 : 1;
	}
}
