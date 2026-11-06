// SPDX-License-Identifier: MIT
/*
 * The contract (README assumption 31): a frame with everything in place draws the triangle;
 * leaving out any one requirement is counted as a violation and, in strict mode, comes out
 * as the chip's does (garbage in the tiles with objects); the same frame in forgiving mode
 * (mbxemu_set_strict(e, 0)) still draws it.
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
	wr(R_TA_CONFIG, 0x80 | (uint32_t)(TRI_W / 16 - 1) << 20 | (uint32_t)(TRI_H / 16 - 1) << 10);
	wr(R_TA_WIDTH, (uint32_t)TRI_W << 16);
	wr(R_TA_HEIGHT, (uint32_t)TRI_H << 16);
	wr(R_TA_REGIONS, REGIONS);
	wr(R_TA_TAILS, TAILS);
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


/* One frame, leaving out the steps in skip (V_* bits); returns the pixels that are not the clear colour */
static struct mbxemu_host HOST;

static int frame(uint32_t skip, int strict, uint32_t *mask)
{
	struct mbxemu_stats st;
	uint32_t stream[128], n, i, tails = TAILS;

	/* a fresh device each time: registers survive a soft reset, and the stats add up */
	mbxemu_free(E);
	E = mbxemu_new(&HOST);
	memset(ram, 0, RAM_SIZE);
	mbxemu_set_strict(E, strict);
	setup_mmu();
	wr(R_INT_CLEAR, 0x7fff);
	mbx_contract_freelist(&IO, PB_TABLE_VA, PB_VA, PB_LAST_PAGE);
	wr(R_FL_FIRST, skip & V_FL_OVERLAP ? PB_FIRST_PAGE : PB_FIRST_PAGE + 1);
	if (skip & V_FL_REGS)
		wr(0x6d8, 0);
	if (skip & V_PB_BASE)
		wr(R_PB_BASE, 0);
	wr(R_FL_BUILD, 1);
	clear_surface();
	wr(R_INT_ENABLE, I_MASTER | I_RENDER | I_TA_DONE | I_TA_OFLOW | I_EVM_DALLOC);
	wr(R_TA_CONFIG, 0x80 | (uint32_t)(TRI_W / 16 - 1) << 20 | (uint32_t)(TRI_H / 16 - 1) << 10);
	wr(R_TA_WIDTH, (uint32_t)TRI_W << 16);
	wr(R_TA_HEIGHT, (uint32_t)TRI_H << 16);
	wr(R_TA_REGIONS, REGIONS);
	if (skip & V_TAILS_ALIGN)
		tails += 0x1000;
	wr(R_TA_TAILS, tails);
	if (!(skip & V_CTX))
		mbx_contract_contexts(&IO, CTX);
	if (!(skip & V_BG))
		mbx_contract_background(&IO, PB_VA, SURF, TRI_W, TRI_W / 16, TRI_H / 16, 0);
	/* the pages the test leaves untouched are zero, so a skipped step leaves zeros */
	memset(ram + dev_phys(REGIONS), 0, 16 * 8);
	if (!(skip & V_REGION_INIT))
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
	wr(R_REGIONS, REGIONS);
	wr(R_RENDER_CTL, 0x30000);
	wr(R_SAMPLING, 0x00020007);
	wr(R_FORMAT, 0xe);
	wr(R_WIDTH, (uint32_t)(TRI_W - 1) << 16);
	wr(R_HEIGHT, (uint32_t)(TRI_H - 1) << 16);
	wr(R_ADDR, SURF);
	wr(R_STRIDE, TRI_W);
	if (!(skip & V_PB_BASE))
		wr(R_PB_BASE, PB_VA);
	if (!(skip & V_REGION_B30))
		mbx_contract_prerender(&IO, REGIONS, TRI_W / 16, TRI_H / 16);
	wr(R_HW_CONTEXT, 0);
	wr(R_KICK, 1);
	mbxemu_stats(E, &st);
	*mask = st.violation_mask;
	return drawn();
}

int main(void)
{
	static const struct { uint32_t bit; const char *what; } cases[] = {
		{ V_FL_OVERLAP, "the free list starting on the background object's page" },
		{ V_FL_REGS, "0x6d8 not set" },
		{ V_PB_BASE, "0x83c not set" },
		{ V_TAILS_ALIGN, "tail pointers not 64 KB aligned" },
		{ V_CTX, "no contexts" },
		{ V_REGION_INIT, "the region array not initialised" },
		{ V_REGION_B30, "bit 30 not set before the render" },
		{ V_BG, "no background object" },
	};
	uint32_t mask;
	int n, k;

	ram = calloc(1, RAM_SIZE);
	HOST = (struct mbxemu_host){ .mem_read = mem_read, .mem_write = mem_write, .irq = irq, .log = log_line };
	E = mbxemu_new(&HOST);

	n = frame(0, 1, &mask);
	CHECK(mask == 0 && n == 1144, "everything in place: no violation, the triangle (mask %#x, %d pixels)", mask, n);
	for (k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
		n = frame(cases[k].bit, 1, &mask);
		CHECK(mask & cases[k].bit, "%s: counted (mask %#x)", cases[k].what, mask);
		CHECK(n != 1144, "%s: strict mode draws garbage, not the triangle (%d pixels)", cases[k].what, n);
		n = frame(cases[k].bit, 0, &mask);
		CHECK(mask & cases[k].bit && n == 1144, "%s: forgiving mode counts it and draws the triangle (%d pixels)",
		      cases[k].what, n);
	}
	printf("t_contract: %d checks, %d failures\n", checks, failures);
	mbxemu_free(E);
	return failures ? 1 : 0;
}
