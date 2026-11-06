// SPDX-License-Identifier: MIT
/*
 * The TA and a sections word without bit 17 (README assumption 30): the chip
 * takes nothing then, and the model hangs the TA the same way. Three frames of
 * the mbxtri triangle on the same device: a normal one, one whose sections word
 * lacks bit 17, and one after a TA start.
 *
 *   make test    (from mbx/)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../emu.h"
#include "../tools/tri_frame.h"

#define RAM_BASE	0x80000000u
#define RAM_SIZE	(8u << 20)

static uint8_t *ram;
static struct mbxemu *E;
static int checks, failures;

#define CHECK(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fputc('\n', stderr);					\
	}								\
} while (0)

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

static void irq(void *o, int level) { }
static void log_line(void *o, const char *line) { if (getenv("T_TA_VERBOSE")) fprintf(stderr, "emu: %s\n", line); }

static void wr(uint32_t reg, uint32_t v) { mbxemu_reg_write(E, reg, v); }
static uint32_t rd(uint32_t reg) { return mbxemu_reg_read(E, reg); }

#define SURF	(RAM_BASE + 0x10000)
#define REGIONS	(RAM_BASE + 0x100000)
#define TAILS	(RAM_BASE + 0x110000)
#define PB	(RAM_BASE + 0x120000)

/* Send a frame's stream: the TA set up and started, the words in; returns INT_STATUS after the stream */
static uint32_t send_frame(uint32_t sections, int start)
{
	uint32_t stream[128], n, i;

	if (start) {
		wr(R_INT_ENABLE, I_MASTER | I_RENDER | I_TA_DONE);
		wr(R_INT_CLEAR, 0x7fff);
		wr(R_TA_CONFIG, 0x80 | (uint32_t)(TRI_W / 16 - 1) << 20 | (uint32_t)(TRI_H / 16 - 1) << 10);
		wr(R_TA_WIDTH, (uint32_t)TRI_W << 16);
		wr(R_TA_HEIGHT, (uint32_t)TRI_H << 16);
		wr(R_TA_REGIONS, REGIONS);
		wr(R_TA_TAILS, TAILS);
		wr(R_TA_CONTEXT_ID, 0);
		wr(R_TA_START, 1);
	}
	n = tri_stream_sections(stream, 0, sections);
	for (i = 0; i < n;) {
		uint32_t type = stream[i] >> 28, count = stream[i] & 0xffff, k;

		for (k = 0; k < count; k++) {
			if (type == CH_CTL)
				mbxemu_ta_ctl_write(E, stream[i + 1 + k]);
			else
				mbxemu_ta_write(E, 0, stream[i + 1 + k]);
		}
		i += 1 + count;
	}
	return rd(R_INT_STATUS);
}

static uint32_t kick_render(void)
{
	wr(R_REGIONS, REGIONS);
	wr(R_RENDER_CTL, 0x30000);
	wr(R_SAMPLING, 0x00020007);
	wr(R_FORMAT, 0xe);
	wr(R_WIDTH, (uint32_t)(TRI_W - 1) << 16);
	wr(R_HEIGHT, (uint32_t)(TRI_H - 1) << 16);
	wr(R_ADDR, SURF);
	wr(R_STRIDE, TRI_W);
	wr(R_PB_BASE, PB);
	wr(R_HW_CONTEXT, 0);
	wr(R_KICK, 1);
	return rd(R_INT_STATUS);
}

static int drawn(void)
{
	const uint32_t *p = (const uint32_t *)(ram + (SURF - RAM_BASE));
	int i, n = 0;

	for (i = 0; i < TRI_W * TRI_H; i++)
		n += p[i] != TRI_CLEAR;
	return n;
}

static void clear_surface(void)
{
	uint32_t *p = (uint32_t *)(ram + (SURF - RAM_BASE));

	for (int i = 0; i < TRI_W * TRI_H; i++)
		p[i] = TRI_CLEAR;
}

int main(void)
{
	struct mbxemu_host host = { .mem_read = mem_read, .mem_write = mem_write, .irq = irq, .log = log_line };
	struct mbxemu_stats st;
	uint32_t status;

	ram = calloc(1, RAM_SIZE);
	E = mbxemu_new(&host);

	/* 1. with bit 17: the TA completes, the render completes, a triangle is drawn */
	clear_surface();
	status = send_frame(TRI_SECTIONS, 1);
	CHECK(status & I_TA_DONE, "TA_COMPLETE with bit 17 set (status %08x)", status);
	CHECK((status >> 16 & 0xff) == 0, "the FIFO is empty (status %08x)", status);
	status = kick_render();
	CHECK(status & I_RENDER, "RENDER_COMPLETE (status %08x)", status);
	CHECK(drawn() > 1000, "a triangle was drawn (%d pixels)", drawn());
	mbxemu_stats(E, &st);
	CHECK(st.prims == 2 && st.ta_hangs == 0, "two primitives (the depth clear and the triangle), no hangs (%u, %u)", st.prims, st.ta_hangs);

	/* 2. bit 17 clear: the TA takes nothing, the FIFO fills, nothing completes, the render does not */
	clear_surface();
	status = send_frame(TRI_SECTIONS & ~SEC_BIT17, 1);
	CHECK(!(status & I_TA_DONE), "no TA_COMPLETE without bit 17 (status %08x)", status);
	CHECK((status >> 16 & 0xff) > 1 && (status >> 16 & 0xff) <= TA_FIFO_WORDS,
	      "the FIFO holds what was sent after the sections word (fill %u)", status >> 16 & 0xff);
	mbxemu_stats(E, &st);
	/* the depth clear (its sections word has bit 17) is taken, the triangle after it is not */
	CHECK(st.ta_hangs == 1 && st.prims == 3, "one hang, and only the clear's primitive new (%u, %u)", st.ta_hangs, st.prims);
	wr(R_INT_CLEAR, 0x7fff);
	status = kick_render();
	CHECK(!(status & I_RENDER), "the render does not complete while the TA is hung (status %08x)", status);
	CHECK(drawn() == 0, "nothing was drawn (%d pixels)", drawn());

	/* more words than the FIFO holds: the fill stops at 59 */
	for (int i = 0; i < 100; i++)
		mbxemu_ta_write(E, 0, 0);
	status = rd(R_INT_STATUS);
	CHECK((status >> 16 & 0xff) == TA_FIFO_WORDS, "the FIFO is full (fill %u)", status >> 16 & 0xff);

	/* 3. a TA start gets it going again */
	status = send_frame(TRI_SECTIONS, 1);
	CHECK(status & I_TA_DONE, "TA_COMPLETE after a restart (status %08x)", status);
	CHECK((status >> 16 & 0xff) == 0, "the FIFO is empty again (status %08x)", status);
	status = kick_render();
	CHECK(status & I_RENDER && drawn() > 1000, "the render completes and draws (%d pixels)", drawn());

	/* 4. a soft reset ends a hang too */
	send_frame(TRI_SECTIONS & ~SEC_BIT17, 1);
	wr(0x080, 0x111);
	wr(0x080, 0);
	status = rd(R_INT_STATUS);
	CHECK((status >> 16 & 0xff) == 0, "a soft reset empties the FIFO (status %08x)", status);

	printf("t_ta: %d checks, %d failures\n", checks, failures);
	mbxemu_free(E);
	return failures ? 1 : 0;
}
