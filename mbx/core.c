// SPDX-License-Identifier: MIT
/*
 * mbxemu: the register file, interrupts, the MMU and device memory, the
 * TA's two hardware contexts and the free list.
 *
 * Everything runs at the access that starts it: a context reset raises
 * TA_CONTEXT before the write returns, an end word TA_COMPLETE, a render
 * kick RENDER_COMPLETE and EVM_DALLOC together. The input FIFO is empty
 * (INT_STATUS bits 16-23 read 0) unless the TA is hung (README assumption
 * 30), so a client's credit is never
 * short.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"

#define CORE_ID		0x01010003	/* MBX1, VGP and MMU present (seen on the N810) */
#define REVISION	0x05010307

/* A contract violation (emu.h V_*): counted every time, logged the first time in a scene */
void emu_violation(struct mbxemu *e, uint32_t bit, const char *what)
{
	if (!(e->viol & bit))
		emu_log(e, "contract: %s%s", what, e->strict ? " (the chip gives garbage)" : "");
	e->viol |= bit;
	e->viol_ever |= bit;
	e->stats.violations++;
	e->stats.violation_mask |= bit;
}

void mbxemu_set_strict(struct mbxemu *e, int strict)
{
	e->strict = strict;
}

void emu_log(struct mbxemu *e, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	if (!e->host.log)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	e->host.log(e->host.opaque, line);
}

/* --- interrupts --- */

/*
 * The line is up while an enabled status bit is set and the enable
 * register's master bit is on. Status bits stay set until cleared
 * (INT_CLEAR); a write to INT_STATUS sets bits, which the services use
 * as a doorbell (8, ISP) and to replay an overflow (32).
 */
static void irq_update(struct mbxemu *e)
{
	int level = (e->enable & I_MASTER) && (e->status & e->enable & 0x7fff);

	if (level != e->irq_level) {
		e->irq_level = level;
		if (e->host.irq)
			e->host.irq(e->host.opaque, level);
	}
}

void emu_raise(struct mbxemu *e, uint32_t bits)
{
	e->status |= bits & 0x7fff;
	irq_update(e);
}

/* --- the MMU and device memory --- */

static void tlb_flush(struct mbxemu *e)
{
	memset(e->tlb, 0, sizeof(e->tlb));
}

static void host_read(struct mbxemu *e, uint32_t phys, void *buf, uint32_t len)
{
	if (e->host.mem_read)
		e->host.mem_read(e->host.opaque, phys, buf, len);
	else
		memset(buf, 0, len);
}

/*
 * A device address to a physical one. With the MMU on, eight page tables
 * (0x1000-0x101c) of 1024 bare physical page addresses map 32 MB; an
 * address past that has no page (-1). The entries are read once into the
 * TLB, which the MMU's enable and the TLB load (0x1020 bit 8, with the
 * page in 0x1024 and its physical page in 0x1028) refresh, as software
 * expects. MMU off: device addresses are physical (assumed).
 */
static int translate(struct mbxemu *e, uint32_t dev, uint32_t *phys)
{
	uint32_t page;

	if (!(e->regs[R_MMU_CTRL / 4] & 1)) {
		*phys = dev;
		return 0;
	}
	if (dev >= DEV_SPACE)
		return -1;
	page = dev >> 12;
	if (!e->tlb[page]) {
		uint32_t entry = 0;

		host_read(e, e->regs[R_MMU_TABLE(page >> 10) / 4] + (page & 1023) * 4, &entry, 4);
		e->tlb[page] = (entry >> 12) + 1;
	}
	*phys = (e->tlb[page] - 1) << 12 | (dev & 0xfff);
	return 0;
}

static struct pcache *pc_slot(struct mbxemu *e, uint32_t page, int *idx)
{
	*idx = (int)(page % PCACHE);
	return e->pc && e->pc->page[*idx] == page + 1 ? e->pc : NULL;
}

void dev_read(struct mbxemu *e, uint32_t dev, void *buf, uint32_t len)
{
	uint8_t *p = buf;

	while (len) {
		uint32_t n = 4096 - (dev & 0xfff), phys;
		int k;

		if (n > len)
			n = len;
		if (e->pc) {
			if (!pc_slot(e, dev >> 12, &k)) {
				if (translate(e, dev & ~0xfffu, &phys))
					memset(e->pc->data[k], 0, 4096);
				else
					host_read(e, phys, e->pc->data[k], 4096);
				e->pc->page[k] = (dev >> 12) + 1;
			}
			memcpy(p, e->pc->data[k] + (dev & 0xfff), n);
		} else if (translate(e, dev, &phys)) {
			memset(p, 0, n);
		} else {
			host_read(e, phys, p, n);
		}
		p += n;
		dev += n;
		len -= n;
	}
}

void dev_write(struct mbxemu *e, uint32_t dev, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;

	while (len) {
		uint32_t n = 4096 - (dev & 0xfff), phys;
		int k;

		if (n > len)
			n = len;
		if (!translate(e, dev, &phys)) {
			/* a page no table entry maps lands on physical page 0: the driver must point unused pages
			 * at a scratch page (README assumption 31) */
			if ((e->regs[R_MMU_CTRL / 4] & 1) && !(phys >> 12))
				emu_violation(e, V_STRAY, "a write through an unmapped page (no scratch page behind it)");
			else if (e->host.mem_write)
				e->host.mem_write(e->host.opaque, phys, p, n);
		}
		if (pc_slot(e, dev >> 12, &k))
			memcpy(e->pc->data[k] + (dev & 0xfff), p, n);
		p += n;
		dev += n;
		len -= n;
	}
}

uint32_t dev_read32(struct mbxemu *e, uint32_t dev)
{
	uint32_t v;

	dev_read(e, dev, &v, 4);
	return v;
}

void dev_write32(struct mbxemu *e, uint32_t dev, uint32_t v)
{
	dev_write(e, dev, &v, 4);
}

void pcache_begin(struct mbxemu *e)
{
	if (!e->pc)
		e->pc = malloc(sizeof(*e->pc));
	if (e->pc)
		memset(e->pc->page, 0, sizeof(e->pc->page));
}

void pcache_end(struct mbxemu *e)
{
	free(e->pc);
	e->pc = NULL;
}

/* --- scenes and the TA's hardware contexts --- */

void scene_clear_prims(struct scene *s)
{
	free(s->prims);
	free(s->states);
	s->prims = NULL;
	s->states = NULL;
	s->nprims = s->cprims = s->nstates = s->cstates = 0;
}

void scene_free(struct scene *s)
{
	scene_clear_prims(s);
	memset(s, 0, sizeof(*s));
}

static struct scene *ctx_slot(struct mbxemu *e)
{
	return &e->ctx[e->regs[R_TA_CONTEXT_ID / 4] & 1];
}

/* The scene the TA adds to: the current context's, if begun and not ended */
struct scene *ta_scene(struct mbxemu *e)
{
	struct scene *s = ctx_slot(e);

	return s->used && !s->complete ? s : NULL;
}

/* 0x81c: a new scene in context 0x810 */
static void ctx_reset(struct mbxemu *e)
{
	struct scene *s = ctx_slot(e);

	scene_free(s);
	s->used = 1;
	e->scene_state = -1;
	emu_trace(e, "TA context %u reset", e->regs[R_TA_CONTEXT_ID / 4] & 1);
}

/*
 * 0x818: the context's scene to TA context memory (0x820), for a scene
 * another render target interrupted, and the macro tiles' next free
 * addresses written there as the N810 writes them (pb_ctx_store()). The
 * rest of what the hardware keeps isn't known: the scene stays ours, filed
 * under that address, with the TA's state blocks. A context with no open
 * scene stores no scene.
 */
static void ctx_store(struct mbxemu *e)
{
	struct scene *s = ctx_slot(e), *n, **p;
	uint32_t key = e->regs[R_TA_CTX_BASE / 4];

	pb_ctx_store(e, key & ~0xffu);
	if (!s->used || s->complete)
		return;
	for (p = &e->stored; *p; p = &(*p)->next)
		if ((*p)->key == key) {
			n = *p;
			*p = n->next;
			scene_free(n);
			free(n);
			break;
		}
	if (!(n = malloc(sizeof(*n))))
		return;
	*n = *s;
	n->ta_state = e->cur;
	n->key = key;
	n->next = e->stored;
	e->stored = n;
	memset(s, 0, sizeof(*s));
	e->scene_state = -1;
	emu_trace(e, "TA context stored to %#x", key);
}

/* 0x814: the scene stored at 0x820 back into context 0x810 */
static void ctx_load(struct mbxemu *e)
{
	struct scene *s = ctx_slot(e), *n, **p;
	uint32_t key = e->regs[R_TA_CTX_BASE / 4];

	scene_free(s);
	for (p = &e->stored; *p; p = &(*p)->next)
		if ((*p)->key == key)
			break;
	if (!(n = *p)) {
		emu_log(e, "TA context load from %#x: nothing stored there; a new scene", key);
		s->used = 1;
	} else {
		*p = n->next;
		*s = *n;
		s->next = NULL;
		e->cur = s->ta_state;
		free(n);
		pb_ctx_load(e, key & ~0xffu);	/* a zeroed entry: a fresh page */
	}
	e->scene_state = -1;
	emu_trace(e, "TA context loaded from %#x", key);
}

/*
 * 0x838: the hardware links pages first..last into the free-list table
 * (16-bit entries: entry p the page after p, the last's too, entry 0 the
 * first) and says EVM_DALLOC; the TA hands them out from there (pb.c).
 */
static void fl_build(struct mbxemu *e)
{
	uint32_t table = e->regs[R_FL_TABLE / 4], first = e->regs[R_FL_FIRST / 4];
	uint32_t last = e->regs[R_FL_LAST / 4], p;
	uint16_t v;
	int built = first && last >= first && last < 0x2000;

	if (built) {
		v = (uint16_t)first;
		dev_write(e, table, &v, 2);
		for (p = first; p <= last; p++) {
			v = (uint16_t)(p + 1);	/* the last's too: 0x82c ends it */
			dev_write(e, table + p * 2, &v, 2);
		}
	}
	pb_fl_built(e, built);
	e->fl_stale = 0;		/* a rebuild repairs what a partial render left */
	emu_raise(e, I_EVM_DALLOC);
}

/* --- registers --- */

uint32_t mbxemu_reg_read(struct mbxemu *e, uint32_t reg)
{
	uint32_t v;

	reg &= 0x3ffc;
	switch (reg) {
	case R_INT_STATUS:
		/* bits 16-23: the FIFO's words, empty unless the TA is hung (ta.c) */
		v = e->status | (e->fifo_fill & 0xff) << 16;
		break;
	case R_INT_ENABLE:
		v = e->enable;
		break;
	case R_INT_CLEAR:
		v = 0;
		break;
	case R_CORE_ID:
		v = CORE_ID;
		break;
	case R_REVISION:
		v = REVISION;
		break;
	case R_ISP_SIG:
		v = e->isp_sig;
		break;
	case R_MMU_CTRL:
		/* enabled (16) follows enable (0); a TLB load is done at once */
		v = (e->regs[reg / 4] & ~0x01010100u) | (e->regs[reg / 4] & 1) << 16;
		break;
	default:
		v = e->regs[reg / 4];
	}
	emu_trace(e, "read  %#06x = %08x", reg, v);
	return v;
}

/* Soft reset (0x080, bits 0, 4, 8 the cores): scenes and the TA's state go,
 * the MMU is off until enabled again; VGP memory and registers stay */
static void soft_reset(struct mbxemu *e)
{
	struct scene *n;

	ta_reset(e);
	so_reset(e);
	twod_reset(e);
	free(e->tq);
	e->tq = NULL;
	e->tqn = e->tqcap = 0;
	e->pbw.stalled = 0;
	e->pbw.job.pending = 0;
	scene_free(&e->ctx[0]);
	scene_free(&e->ctx[1]);
	while ((n = e->stored)) {
		e->stored = n->next;
		scene_free(n);
		free(n);
	}
	e->regs[R_MMU_CTRL / 4] = 0;
	tlb_flush(e);
}

static void tq_drain(struct mbxemu *e);

void mbxemu_reg_write(struct mbxemu *e, uint32_t reg, uint32_t v)
{
	reg &= 0x3ffc;
	emu_trace(e, "write %#06x = %08x", reg, v);
	switch (reg) {
	case R_INT_STATUS:
		emu_raise(e, v);
		return;
	case R_INT_ENABLE:
		e->enable = v;
		irq_update(e);
		return;
	case R_INT_CLEAR:
		e->status &= ~v;
		irq_update(e);
		return;
	case R_CORE_ID:
	case R_REVISION:
		return;
	}
	e->regs[reg / 4] = v;
	switch (reg) {
	case R_SOFT_RESET:
		if (v)
			soft_reset(e);
		break;
	case R_KICK:
		if (v & 1) {
			render_kick(e);
			pb_render(e);
		}
		break;
	case R_TA_START:
		if (v & 1) {
			if (!e->pbw.stalled)
				ta_start(e);
			else if (!pb_resume(e))
				tq_drain(e);
		}
		break;
	case R_TA_CLOSE:
		if (v & 1) {
			int stalled = e->pbw.stalled;

			if (stalled)
				pb_close(e);
			ta_end_scene(e, 1);
			if (stalled)
				pb_closed(e);
		}
		break;
	case R_TA_CTX_RESET:
		if (v & 1) {
			ctx_reset(e);
			emu_raise(e, I_TA_CONTEXT);
		}
		break;
	case R_TA_CTX_STORE:
		if (v & 1) {
			ctx_store(e);
			emu_raise(e, I_TA_CONTEXT | I_B0);
		}
		break;
	case R_TA_CTX_LOAD:
		if (v & 1) {
			ctx_load(e);
			emu_raise(e, I_TA_CONTEXT);
		}
		break;
	case R_FL_BUILD:
		if (v & 1)
			fl_build(e);
		break;
	case R_MMU_CTRL:
		if (v & 0x100) {
			uint32_t page = e->regs[R_MMU_TLB_PAGE / 4];

			if (page < (DEV_SPACE >> 12))
				e->tlb[page] = e->regs[R_MMU_TLB_PHYS / 4] + 1;
		} else {
			tlb_flush(e);
		}
		e->regs[reg / 4] &= ~0x01010100u;
		break;
	default:
		if (reg >= R_MMU_TABLE(0) && reg <= R_MMU_TABLE(7))
			tlb_flush(e);
	}
}

/* --- the window --- */

/*
 * The TA's input while it is stalled (TA_OFLOW: the parameter buffer out
 * of pages): kept, in order, for when it is resumed. On the N810 the
 * port holds the bus instead.
 */
static void tq_push(struct mbxemu *e, uint32_t what, uint32_t v)
{
	if (e->tqn == e->tqcap) {
		size_t cap = e->tqcap ? 2 * e->tqcap : 1024;
		uint32_t (*q)[2] = realloc(e->tq, cap * sizeof(*q));

		if (!q) {
			emu_log(e, "TA: input lost while stalled (no memory)");
			return;
		}
		e->tq = q;
		e->tqcap = cap;
	}
	e->tq[e->tqn][0] = what;
	e->tq[e->tqn][1] = v;
	e->tqn++;
}

/* Feed what was kept, until it is all in or the TA stalls again */
static void tq_drain(struct mbxemu *e)
{
	size_t k;

	for (k = 0; k < e->tqn && !e->pbw.stalled; k++) {
		if (e->tq[k][0] == 4)
			ta_ctl(e, e->tq[k][1]);
		else
			ta_word(e, e->tq[k][0], e->tq[k][1]);
	}
	memmove(e->tq, e->tq + k, (e->tqn - k) * sizeof(*e->tq));
	e->tqn -= k;
}

void mbxemu_ta_write(struct mbxemu *e, uint32_t off, uint32_t v)
{
	if (e->pbw.stalled)
		tq_push(e, off >> 2 & 3, v);
	else
		ta_word(e, off >> 2 & 3, v);
}

void mbxemu_ta_ctl_write(struct mbxemu *e, uint32_t v)
{
	if (e->pbw.stalled)
		tq_push(e, 4, v);
	else
		ta_ctl(e, v);
}

void mbxemu_2d_write(struct mbxemu *e, uint32_t v)
{
	twod_word(e, v);
}

uint32_t mbxemu_read(struct mbxemu *e, uint32_t off)
{
	if (off < MBXEMU_REGS_SIZE)
		return mbxemu_reg_read(e, off);
	return 0;	/* the slave ports read nothing (assumed) */
}

void mbxemu_write(struct mbxemu *e, uint32_t off, uint32_t v)
{
	if (off < MBXEMU_REGS_SIZE)
		mbxemu_reg_write(e, off, v);
	else if (off >= MBXEMU_TA_PORT && off < MBXEMU_TA_PORT + MBXEMU_PORT_SIZE)
		mbxemu_ta_write(e, off - MBXEMU_TA_PORT, v);
	else if (off >= MBXEMU_2D_PORT && off < MBXEMU_2D_PORT + MBXEMU_PORT_SIZE)
		mbxemu_2d_write(e, v);
	else if (off >= MBXEMU_TACTL_PORT && off < MBXEMU_TACTL_PORT + MBXEMU_PORT_SIZE)
		mbxemu_ta_ctl_write(e, v);
}

void mbxemu_set_trace(struct mbxemu *e, int level)
{
	e->trace = level;
}

void mbxemu_stats(const struct mbxemu *e, struct mbxemu_stats *s)
{
	*s = e->stats;
}

void mbxemu_reset(struct mbxemu *e)
{
	struct mbxemu_host host = e->host;
	int trace = e->trace, strict = e->strict;

	soft_reset(e);
	pcache_end(e);
	memset(e, 0, sizeof(*e));
	e->host = host;
	e->trace = trace;
	e->strict = strict;
	e->regs[R_TA_CONFIG / 4] = 0x80;
	e->scene_state = -1;
	ta_reset(e);
	so_reset(e);
	twod_reset(e);
	if (host.irq)
		host.irq(host.opaque, 0);
}

struct mbxemu *mbxemu_new(const struct mbxemu_host *host)
{
	struct mbxemu *e = calloc(1, sizeof(*e));

	if (!e)
		return NULL;
	e->host = *host;
	e->strict = 1;
	mbxemu_reset(e);
	return e;
}

void mbxemu_free(struct mbxemu *e)
{
	if (!e)
		return;
	soft_reset(e);
	pcache_end(e);
	free(e);
}
