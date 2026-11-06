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
/* the test's own writes through an unmapped page go nowhere (the model's would be a stray write) */
static int dev_mapped(uint32_t va)
{
	const uint32_t *pt = (const uint32_t *)(ram + PT_PHYS + ((va >> 12) >> 10) * PAGE);

	return pt[(va >> 12) & 1023] != 0;
}
static void c_wr32(void *o, uint32_t va, uint32_t v) { if (dev_mapped(va)) memcpy(ram + dev_phys(va), &v, 4); }
static uint32_t c_rd32(void *o, uint32_t va) { uint32_t v = 0; if (dev_mapped(va)) memcpy(&v, ram + dev_phys(va), 4); return v; }
static void c_reg(void *o, uint32_t reg, uint32_t v);
static const struct mbx_io IO = { .wr32 = c_wr32, .rd32 = c_rd32, .reg = c_reg };

static void irq(void *o, int level) { }
static void log_line(void *o, const char *line) { if (getenv("T_PB_VERBOSE")) fprintf(stderr, "emu: %s\n", line); }

static void wr(uint32_t reg, uint32_t v) { mbxemu_reg_write(E, reg, v); }
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

static struct mbxemu_host HOST;
/* test-only variants of a frame, on top of the skipped steps */
enum { X_NONE, X_CTX_UNALIGNED, X_NO_END, X_BREAK_LIST, X_CTX_UNMAPPED, X_STALE, X_MARKERS, X_NO_BASE, X_GRID,
       X_FMT, X_ENTRY, X_ALIGN4K, X_NO_CLEAR };
static int variant;

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
	mbx_contract_pb_fill(&IO, PB_VA, PB_PAGES);
	mbx_contract_freelist(&IO, PB_TABLE_VA, PB_VA, PB_LAST_PAGE);
	wr(R_FL_FIRST, skip & V_FL_OVERLAP ? PB_FIRST_PAGE : PB_FIRST_PAGE + 1);
	if (skip & V_FL_REGS)
		wr(0x6d8, 0);
	if (skip & V_PB_BASE)
		wr(R_PB_BASE, 0);
	wr(R_FL_BUILD, 1);
	clear_surface();
	wr(R_INT_ENABLE, I_MASTER | I_RENDER | I_TA_DONE | I_TA_OFLOW | I_EVM_DALLOC);
	wr(R_TA_CONFIG, (variant == X_NO_BASE ? 0x80 : MBX_TA_BASE) | (uint32_t)(TRI_W / 16 - 1) << 20 | (uint32_t)(TRI_H / 16 - 1) << 10);
	wr(R_TA_WIDTH, (uint32_t)(variant == X_GRID ? TRI_W + 32 : TRI_W) << 16);
	wr(R_TA_HEIGHT, (uint32_t)TRI_H << 16);
	mbx_contract_ta_setup(&IO);
	wr(R_TA_REGIONS, REGIONS);
	if (skip & V_TAILS_ALIGN)
		tails += 0x1000;
	wr(R_TA_TAILS, tails);
	if (!(skip & V_CTX)) {
		uint32_t ctx = variant == X_CTX_UNALIGNED ? CTX + 0x1000 : variant == X_ALIGN4K ? CTX + 0x100 : variant == X_CTX_UNMAPPED ? 0x70000 : CTX;

		mbx_contract_contexts(&IO, ctx);
	}
	if (variant == X_MARKERS) {
		c_wr32(NULL, CTX + 0x4400, 0xdeadbeef);		/* inside the EVM context's overrun */
		c_wr32(NULL, CTX + 0x4000 + 520 + 1024 + 8, 0xfeedf00d);	/* past it */
	}
	if (skip & V_TAILS_ALIGN)
		c_wr32(NULL, TAILS + 0x10, 0xabcdef01);		/* where an unaligned tail pointer rounds down to */
	if (!(skip & V_BG))
		mbx_contract_background(&IO, PB_VA, SURF, TRI_W, TRI_W / 16, TRI_H / 16, 0);
	/* the pages the test leaves untouched are zero, so a skipped step leaves zeros */
	memset(ram + dev_phys(REGIONS), 0, 16 * 8);
	if (!(skip & V_REGION_INIT))
		mbx_contract_regions(&IO, REGIONS, TRI_W / 16, TRI_H / 16);
	mbx_contract_tails(&IO, TAILS, 4096);
	wr(R_TA_CONTEXT_ID, 0);
	wr(R_TA_START, 1);
	tri_with_clear = variant != X_NO_CLEAR;
	n = tri_stream(stream, 0);
	for (i = 0; i < n;) {
		uint32_t type = stream[i] >> 28, count = stream[i] & 0xffff, k;

		for (k = 0; k < count; k++) {
			if ((variant == X_NO_END || variant == X_STALE) && i + 1 + k == n - 1)
				break;				/* the end word withheld */
			if (type == CH_CTL)
				mbxemu_ta_ctl_write(E, stream[i + 1 + k]);
			else
				mbxemu_ta_write(E, 0, stream[i + 1 + k]);
		}
		i += 1 + count;
	}
	if (variant == X_STALE)
		wr(0x808, 1);					/* a close: a partial render's */
	if (variant == X_ENTRY) {
		c_wr32(NULL, REGIONS + 8 * 5, 0x00000005 | 0x40000000);	/* entry 5: no bit 25 ... */
		c_wr32(NULL, REGIONS + 8 * 5 + 4, 0);			/* ... and no list */
	}
	if (variant == X_BREAK_LIST) {
		uint32_t head = c_rd32(NULL, REGIONS + 4);	/* the first tile's list */

		c_wr32(NULL, head, 0);				/* an empty entry at its start */
	}
	wr(R_REGIONS, REGIONS);
	wr(R_RENDER_CTL, 0x30000);
	wr(R_SAMPLING, 0x00020007);
	wr(R_FORMAT, variant == X_FMT ? 9 : 0xe);
	wr(R_WIDTH, (uint32_t)(TRI_W - 1) << 16);
	wr(R_HEIGHT, (uint32_t)(TRI_H - 1) << 16);
	wr(R_ADDR, SURF);
	wr(R_STRIDE, TRI_W);
	mbx_contract_render_regs(&IO, TRI_W / 16, TRI_H / 16);
	if (skip & V_TILES)
		wr(0x860, 0x00030002);
	if (!(skip & V_PB_BASE))
		wr(R_PB_BASE, PB_VA);
	if (!(skip & V_REGION_B30))
		mbx_contract_prerender(&IO, REGIONS, TRI_W / 16, TRI_H / 16);
	wr(R_HW_CONTEXT, 0);
	wr(R_KICK, 1);
	if (variant == X_STALE) {		/* the partial render again, the list not rebuilt */
		wr(R_TA_START, 1);
		wr(0x808, 1);
		mbx_contract_prerender(&IO, REGIONS, TRI_W / 16, TRI_H / 16);
		wr(R_KICK, 1);
	}
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
	CHECK((mask & ~V_SOFT) == 0 && n == 1144, "everything in place: no hard violation, the triangle (mask %#x, %d pixels)", mask, n);
	/* what this stack does not do yet (values not known): the registers it never writes are counted, softly */
	CHECK(mask & V_REGS, "the render registers this stack never writes are counted (mask %#x)", mask);
	for (k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
		n = frame(cases[k].bit, 1, &mask);
		CHECK(mask & cases[k].bit, "%s: counted (mask %#x)", cases[k].what, mask);
		CHECK(n != 1144, "%s: strict mode draws garbage, not the triangle (%d pixels)", cases[k].what, n);
		n = frame(cases[k].bit, 0, &mask);
		CHECK(mask & cases[k].bit && n == 1144, "%s: forgiving mode counts it and draws the triangle (%d pixels)",
		      cases[k].what, n);
	}
	/* the rest of the contract: what each costs and what the chip does about it */
	variant = X_CTX_UNALIGNED; n = frame(0, 1, &mask);
	CHECK(mask & V_CTX && n != 1144, "contexts not 16 KB aligned: counted, garbage (mask %#x, %d)", mask, n);
	variant = X_NO_END; frame(0, 1, &mask);
	CHECK(mask & V_RENDER_EARLY, "a render before the scene's end word: counted (mask %#x)", mask);
	variant = X_BREAK_LIST; n = frame(0, 1, &mask);
	CHECK(mask & V_LIST && n != 1144, "a damaged list: counted, garbage (mask %#x, %d)", mask, n);
	variant = X_CTX_UNMAPPED; frame(0, 1, &mask);
	CHECK(mask & V_STRAY, "a context behind unmapped pages: the stray write is counted (mask %#x)", mask);
	variant = X_STALE; frame(0, 1, &mask);
	CHECK(mask & V_FL_STALE, "two partial renders without a free list rebuild: counted (mask %#x)", mask);
	variant = X_NONE;
	n = frame(V_FL_STALE, 1, &mask);	/* nothing skipped that matters here: the free list is rebuilt */
	CHECK(!(mask & V_FL_STALE), "a full frame after the stale case starts clean (mask %#x)", mask);
	/* the effects in memory: an unaligned tail pointer zeroes at the rounded-down address; the EVM
	 * context is written past its 520 bytes */
	frame(V_TAILS_ALIGN, 1, &mask);
	CHECK(c_rd32(NULL, TAILS + 0x10) == 0, "an unaligned tail pointer zeroed the tail area at the rounded-down address");
	variant = X_MARKERS; frame(0, 1, &mask);
	CHECK(c_rd32(NULL, CTX + 0x4400) == 0 && c_rd32(NULL, CTX + 0x4000 + 520 + 1024 + 8) == 0xfeedf00d,
	      "the EVM context's overrun zeroed what lay behind it, and no more");
	/* the soft checks count and never garble the picture */
	variant = X_NO_BASE; n = frame(0, 1, &mask);
	CHECK(mask & V_TA_BASE && n == 1144, "0x85c without the base bits 0xe1: counted, the triangle still drawn (mask %#x, %d)", mask, n);
	variant = X_NONE; frame(0, 1, &mask);
	CHECK(!(mask & V_TA_BASE), "0x85c with the base bits: not counted (mask %#x)", mask);
	variant = X_GRID; n = frame(0, 1, &mask);
	CHECK(mask & V_GRID, "0x85c's grid and 0x84c's size disagree: counted (mask %#x)", mask);
	variant = X_FMT; n = frame(0, 1, &mask);
	CHECK(mask & V_FMT, "the render's format and the background object disagree: counted (mask %#x)", mask);
	variant = X_ENTRY; n = frame(0, 1, &mask);
	CHECK(mask & V_REGION_ENTRY && n != 1144, "a region entry with neither a list nor bit 25: counted, garbage (mask %#x, %d)", mask, n);
	variant = X_ALIGN4K; frame(0, 1, &mask);
	CHECK(mask & V_ALIGN_SOFT, "the contexts not 16 KB aligned (soft): counted (mask %#x)", mask);
	variant = X_NONE;
	n = frame(V_TILES, 1, &mask);
	CHECK(mask & V_TILES && n == 1144, "0x860 not matching 0x85c: counted (soft), the picture is fine (mask %#x, %d)", mask, n);
	variant = X_NO_CLEAR; n = frame(0, 1, &mask);
	CHECK(mask & V_DEPTH_CLEAR && n != 1144, "no depth-only clear: counted, the depth starts as NaN and the triangle fails its test (mask %#x, %d)", mask, n);
	variant = X_NO_CLEAR; n = frame(0, 0, &mask);
	CHECK(mask & V_DEPTH_CLEAR && n == 1144, "no depth-only clear, forgiving mode: counted, the triangle is drawn (%d)", n);
	variant = X_NONE;
	printf("t_contract: %d checks, %d failures\n", checks, failures);
	mbxemu_free(E);
	return failures ? 1 : 0;
}
