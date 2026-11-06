// SPDX-License-Identifier: MIT
/*
 * The parameter buffer through the MMU, as the kernel driver sets it up
 * (mbx/drm/mbx_drv.h): the free list's table and 1024 pages mapped at the top of
 * the 32 MB address space, the list built (0x824-0x82c, 0x838), frames binned
 * into it, pages given back by each render, and an overflow when the list is
 * too short.
 *
 *   make test    (from mbx/)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../emu.h"
#include "../lib/mbx_contract.h"
#include "../tools/tri_frame.h"

#define RAM_BASE	0x80000000u
#define RAM_SIZE	(8u << 20)
#define PT_PHYS		0x1000u			/* eight page tables */
#define PB_PHYS		0x200000u		/* the table, then the pages */

/* the kernel driver's layout (mbx/drm/mbx_drv.h) */
#define PAGE		4096u
#define VM_SIZE		(32u << 20)
#define PB_PAGES	1024u
#define PB_LAST_PAGE	((VM_SIZE >> 12) - 1)
#define PB_FIRST_PAGE	(PB_LAST_PAGE + 1 - PB_PAGES)
#define PB_TABLE_SIZE	((VM_SIZE >> 12) * 2)
#define PB_VA		(PB_FIRST_PAGE << 12)
#define PB_TABLE_VA	(PB_VA - PB_TABLE_SIZE)

/* device addresses of the small buffers (mapped one to one on RAM_BASE) */
#define SURF		0x10000u
#define REGIONS		0x20000u
#define TAILS		0x30000u
#define CTX		0x50000u		/* the TA and EVM contexts: 32 KB, 16 KB aligned */

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

/* device memory as the MBX sees it: through the page tables the test set up */
static uint32_t dev_phys(uint32_t va)
{
	const uint32_t *pt = (const uint32_t *)(ram + PT_PHYS + ((va >> 12) >> 10) * PAGE);

	return (pt[(va >> 12) & 1023] - RAM_BASE) + (va & (PAGE - 1));
}
static void c_wr32(void *o, uint32_t va, uint32_t v) { memcpy(ram + dev_phys(va), &v, 4); }
static uint32_t c_rd32(void *o, uint32_t va) { uint32_t v; memcpy(&v, ram + dev_phys(va), 4); return v; }
static void c_reg(void *o, uint32_t reg, uint32_t v);
static const struct mbx_io IO = { .wr32 = c_wr32, .rd32 = c_rd32, .reg = c_reg };

static void irq(void *o, int level) { }
static void log_line(void *o, const char *line) { if (getenv("T_PB_VERBOSE")) fprintf(stderr, "emu: %s\n", line); }

static void wr(uint32_t reg, uint32_t v) { mbxemu_reg_write(E, reg, v); }
static uint32_t rd(uint32_t reg) { return mbxemu_reg_read(E, reg); }
static void c_reg(void *o, uint32_t reg, uint32_t v) { wr(reg, v); }

static void map_page(uint32_t dev_page, uint32_t phys)
{
	uint32_t *pt = (uint32_t *)(ram + PT_PHYS + (dev_page >> 10) * PAGE);

	pt[dev_page & 1023] = phys;
}

static void setup_mmu(void)
{
	uint32_t i;

	for (i = 0x10; i < 0x58; i++)			/* the small buffers: device 0x10000.. on RAM_BASE + the same */
		map_page(i, RAM_BASE + i * PAGE);
	/* the free list's table and the parameter buffer's pages: one object, contiguous in both */
	for (i = 0; i < PB_TABLE_SIZE / PAGE + PB_PAGES; i++)
		map_page((PB_TABLE_VA >> 12) + i, RAM_BASE + PB_PHYS + i * PAGE);
	for (i = 0; i < 8; i++)
		wr(R_MMU_TABLE(i), RAM_BASE + PT_PHYS + i * PAGE);
	wr(R_MMU_CTRL, 1);
}

/* Build the free list over pages first..last: true if EVM_DALLOC came */
static int build_list(uint32_t first, uint32_t last)
{
	wr(R_INT_CLEAR, 0x7fff);
	mbx_contract_pb_fill(&IO, PB_VA, PB_PAGES);
	mbx_contract_freelist(&IO, PB_TABLE_VA, PB_VA, last);
	wr(R_FL_FIRST, first);
	wr(R_FL_BUILD, 1);
	return !!(rd(R_INT_STATUS) & I_EVM_DALLOC);
}

static uint32_t send_frame(void)
{
	uint32_t stream[128], n, i;

	wr(R_INT_ENABLE, I_MASTER | I_RENDER | I_TA_DONE | I_TA_OFLOW | I_EVM_DALLOC);
	wr(R_INT_CLEAR, 0x7fff);
	wr(R_TA_CONFIG, MBX_TA_BASE | (uint32_t)(TRI_W / 16 - 1) << 20 | (uint32_t)(TRI_H / 16 - 1) << 10);
	wr(R_TA_WIDTH, (uint32_t)TRI_W << 16);
	wr(R_TA_HEIGHT, (uint32_t)TRI_H << 16);
	wr(R_TA_REGIONS, REGIONS);
	wr(R_TA_TAILS, TAILS);
	mbx_contract_ta_setup(&IO);
	mbx_contract_contexts(&IO, CTX);	/* before the first TA: the TA and EVM contexts */
	mbx_contract_background(&IO, PB_VA, SURF, TRI_W, TRI_W / 16, TRI_H / 16, 0);
	mbx_contract_regions(&IO, REGIONS, TRI_W / 16, TRI_H / 16);
	mbx_contract_tails(&IO, TAILS, 4096);
	wr(R_TA_CONTEXT_ID, 0);
	wr(R_TA_START, 1);
	n = tri_stream(stream, 0);
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
	wr(R_PB_BASE, PB_VA);
	mbx_contract_prerender(&IO, REGIONS, TRI_W / 16, TRI_H / 16);
	wr(R_HW_CONTEXT, 0);
	wr(R_KICK, 1);
	return rd(R_INT_STATUS);
}

static int drawn(void)
{
	const uint32_t *p = (const uint32_t *)(ram + SURF);
	int i, n = 0;

	for (i = 0; i < TRI_W * TRI_H; i++)
		n += p[i] != TRI_CLEAR;
	return n;
}

static void clear_surface(void)
{
	uint32_t *p = (uint32_t *)(ram + SURF);

	for (int i = 0; i < TRI_W * TRI_H; i++)
		p[i] = TRI_CLEAR;
}

/* The region array the TA wrote: tiles with a list pointer, tiles flagged empty (bit 25) */
static void regions(int *listed, int *empty)
{
	const uint32_t *r = (const uint32_t *)(ram + REGIONS);

	*listed = *empty = 0;
	for (int k = 0; k < 16; k++) {
		if (r[2 * k + 1])
			(*listed)++;
		if (r[2 * k] & 0x02000000)
			(*empty)++;
	}
}

static int pb_words_used(void)
{
	const uint32_t *p = (const uint32_t *)(ram + PB_PHYS + PB_TABLE_SIZE);
	int n = 0;

	for (uint32_t i = 0; i < PB_PAGES * PAGE / 4; i++)
		n += p[i] != 0;
	return n;
}

int main(void)
{
	struct mbxemu_host host = { .mem_read = mem_read, .mem_write = mem_write, .irq = irq, .log = log_line };
	uint32_t status;
	int listed, empty, f;

	ram = calloc(1, RAM_SIZE);
	E = mbxemu_new(&host);
	setup_mmu();

	/* the list builds and the TA bins into it */
	CHECK(build_list(PB_FIRST_PAGE + 1, PB_LAST_PAGE), "EVM_DALLOC after the free list build");
	clear_surface();
	status = send_frame();
	CHECK(status & I_TA_DONE && !(status & I_TA_OFLOW), "TA_COMPLETE, no overflow (status %08x)", status);
	regions(&listed, &empty);
	/* every tile of a macro tile the TA touched gets a list (assumption 26): 64x64 is one macro tile of 4x4 */
	CHECK(listed == 16 && empty == 0, "region array: %d tiles with a list, %d flagged empty", listed, empty);
	CHECK(pb_words_used() > 20, "the TA wrote the parameter buffer (%d words)", pb_words_used());
	status = kick_render();
	CHECK(status & I_RENDER, "RENDER_COMPLETE (status %08x)", status);
	CHECK(drawn() == 1144, "the triangle is drawn as without a parameter buffer (%d pixels)", drawn());

	/* a render gives its pages back: many frames on the same list never run it dry */
	for (f = 0; f < 40; f++) {
		clear_surface();
		status = send_frame();
		if (!(status & I_TA_DONE) || (status & I_TA_OFLOW))
			break;
		kick_render();
	}
	CHECK(f == 40, "40 frames on one list (stopped at %d, status %08x)", f, status);

	/* a list too short for a frame: the TA overflows and takes no more */
	wr(R_SOFT_RESET, 0x111);
	wr(R_SOFT_RESET, 0);
	setup_mmu();
	CHECK(build_list(PB_FIRST_PAGE + 1, PB_FIRST_PAGE + 2), "a two page list builds");
	status = send_frame();
	CHECK(status & I_TA_OFLOW && !(status & I_TA_DONE), "TA_OFLOW on a list that is too short (status %08x)", status);

	printf("t_pb: %d checks, %d failures\n", checks, failures);
	mbxemu_free(E);
	return failures ? 1 : 0;
}
