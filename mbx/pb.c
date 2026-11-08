/* SPDX-License-Identifier: MIT */
/*
 * mbxemu: the parameter buffer as the N810's TA writes it, next to the scene the model keeps
 * for its render. README assumption 26 lists what is guessed.
 *
 * Pages come off the free list as needed. A draw's vertices go out in
 * blocks of up to six (two list triangles, or a strip's shared vertices;
 * fans, points and lines as strips, pb_vertex()):
 * the alpha test word, each texture unit's words A, B, C, the state word,
 * then the x, y pairs (pixels), the z's and the rhw's, then each vertex's
 * packed ARGB colour and texture coordinates. The first block after a TA
 * start is written once, to a page those blocks share; every later one
 * is copied into the page of each macro tile its triangles' bounding
 * boxes (in tiles) touch, where a bump pointer hands out the block,
 * then 16-word list blocks for tiles that have none yet or whose list is
 * full. A list entry
 * points at a block: bits 30-25 the block's triangles in the tile (by the
 * vertex each starts at), 21-24 its vertex count + 1 (+ 4 a texture unit),
 * the low bits its word address; a full list's 16th word links on
 * (0xe0000000 | the next block's byte address); 0xf0000000 ends it.
 * Out of pages, the TA stalls with TA_OFLOW until resumed (job_run()).
 */
#include <math.h>
#include <string.h>

#include "emu.h"

#define LIST_WORDS	16
#define LIST_END	0xf0000000u
#define LINK		0xe0000000u

/*
 * The free list, as the N810 keeps it: entry 0 holds the page last handed out (a build
 * puts the first page's predecessor there: 0, whose entry is the first),
 * each page's entry the next; the list's last page (0x82c at the build,
 * then the last page given back) is kept back, so the TA overflows when it
 * is next, and only a close takes it. A render gives each macro tile's
 * pages back at the end, in order (pb_render()).
 */
static uint16_t fl_entry(struct mbxemu *e, uint32_t p)
{
	uint16_t v = 0;

	dev_read(e, e->regs[R_FL_TABLE / 4] + p * 2, &v, 2);
	return v;
}

static void fl_set(struct mbxemu *e, uint32_t p, uint16_t v)
{
	dev_write(e, e->regs[R_FL_TABLE / 4] + p * 2, &v, 2);
}

/* A page off the free list, or 0 */
static uint32_t page_alloc(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;
	uint32_t p;

	if (!w->fl_ok)
		return 0;
	p = fl_entry(e, w->cur);
	if (!p || (p == w->tail && !w->closing))
		return 0;
	w->cur = p;
	w->taken++;
	fl_set(e, 0, (uint16_t)p);
	return p << 12;
}

/* A macro tile's page, for pb_render() to give back */
static void mt_page_taken(struct mbxemu *e, uint32_t m, uint32_t a)
{
	struct pbw *w = &e->pbw;

	if (w->mt_npages[m] < PB_MT_PAGES)
		w->mt_pages[m][w->mt_npages[m]++] = (uint16_t)(a >> 12);
	else
		emu_log(e, "parameter buffer: macro tile %u has too many pages to track", m);
}

/* The pages on the free list from the one after the last handed out to the kept-back last page (a debugging count) */
static uint32_t pb_free_count(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;
	uint32_t p = w->cur, n = 0;

	while (n < 4096) {
		p = fl_entry(e, p);
		if (!p)
			break;
		n++;
		if (p == w->tail)
			break;
	}
	return n;
}

/*
 * A render: each macro tile's pages back on the free list, macro tile by
 * macro tile, after its last page; the shared page stays (after the
 * first partial render the free list goes on 8, 2, 3, ... 7, 8 with page 1
 * left out). The TA keeps each macro tile's next free address
 * all the same, so a macro tile goes on in a page the list hands out
 * again (the second quad's lists after 0x4300 on a page of others')
 */
void pb_render(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;
	uint32_t m, k;

	if (!w->on || !w->fl_ok)
		return;
	uint32_t given = 0;

	for (m = 0; m < w->nmx * w->nmy; m++) {
		for (k = 0; k < w->mt_npages[m]; k++) {
			if (w->mt_pages[m][k] == w->cur)
				continue;	/* the page the list's read position is on keeps its link to the rest of the list; linking it on would cut that off */
			fl_set(e, w->tail, w->mt_pages[m][k]);
			w->tail = w->mt_pages[m][k];
			given++;
		}
		w->mt_npages[m] = 0;
	}
	if (e->trace)
		emu_log(e, "parameter buffer: the render gave back %u pages (%u taken since the scene start); %u are on the free list", given, w->taken, pb_free_count(e));
}

/*
 * A render whose 0x860 does not match the TA's grid: the deallocation frees the wrong pages, and the free
 * list leaks until the TA runs dry (found during device testing: it ends in a hang). The model loses the
 * pages: they are not given back.
 */
void pb_render_leak(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;
	uint32_t m;

	if (!w->on || !w->fl_ok)
		return;
	for (m = 0; m < w->nmx * w->nmy; m++)
		w->mt_npages[m] = 0;
}

/* After a free-list build (0x838): built 0 when the pages were past the
 * table (the model's tests without the MMU, whose buffer has physical
 * page numbers): no list, the writer is off */
void pb_fl_built(struct mbxemu *e, int built)
{
	e->pbw.cur = 0;
	e->pbw.tail = e->regs[R_FL_LAST / 4];
	e->pbw.fl_ok = built;
	if (e->trace)
		emu_log(e, "parameter buffer: free list built, %u pages", pb_free_count(e));
}

void pb_scene_start(struct mbxemu *e, uint32_t config)
{
	struct pbw *w = &e->pbw;
	uint32_t cur = w->cur, tail = w->tail;
	int ok = w->fl_ok;

	memset(w, 0, sizeof(*w));
	w->cur = cur;
	w->tail = tail;
	w->fl_ok = ok;
	w->on = ok;
	tile_grid(config, &w->tx, &w->ty, &w->mx, &w->my);
	w->nmx = (w->tx + w->mx - 1) / w->mx;
	w->nmy = (w->ty + w->my - 1) / w->my;
	if (w->tx * w->ty > PB_TILES || w->nmx * w->nmy > PB_MTS)
		w->on = 0;
}

/*
 * The TA context as the N810 stores it (0x818 to 0x820's address): 65
 * words, a row of 8 per macro-tile row (entry mty * 8 + mtx), each a macro
 * tile's next free address as 0x00800000 | its word address (0 for a macro
 * tile without a page), words 32-63 zero on a 4-row screen, word 64 the
 * shared blocks' next free address (0x00800900 is page 2 + 0x400).
 * Loaded (0x814), the entries are taken back: a zero starts the macro tile
 * on a fresh page, as software's re-seed after an overflow's partial
 * render asks.
 */
#define PB_CTX_WORDS	65

static uint32_t ctx_word(uint32_t page, uint32_t bump)
{
	return page ? 0x00800000 | (page + bump) >> 2 : 0;
}

void pb_ctx_store(struct mbxemu *e, uint32_t base)
{
	struct pbw *w = &e->pbw;
	uint32_t k, m;

	for (k = 0; k < PB_CTX_WORDS - 1; k++) {
		uint32_t mx = k % 8, my = k / 8;

		m = my * w->nmx + mx;
		dev_write32(e, base + 4 * k, w->on && mx < w->nmx && my < w->nmy ?
			    ctx_word(w->mt_page[m], w->mt_bump[m]) : 0);
	}
	dev_write32(e, base + 4 * k, w->on ? ctx_word(w->gpage, w->gbump) : 0);
}

void pb_ctx_load(struct mbxemu *e, uint32_t base)
{
	struct pbw *w = &e->pbw;
	uint32_t mx, my, v;

	if (!w->on)
		return;
	for (my = 0; my < w->nmy && my < 8; my++)
		for (mx = 0; mx < w->nmx && mx < 8; mx++) {
			uint32_t m = my * w->nmx + mx;

			v = dev_read32(e, base + 4 * (my * 8 + mx));
			w->mt_page[m] = v & 0x00800000 ? (v & 0x3fffff) << 2 & ~0xfffu : 0;
			w->mt_bump[m] = v & 0x00800000 ? (v & 0x3fffff) << 2 & 0xfff : 0;
		}
	v = dev_read32(e, base + 4 * (PB_CTX_WORDS - 1));
	w->gpage = v & 0x00800000 ? (v & 0x3fffff) << 2 & ~0xfffu : 0;
	w->gbump = v & 0x00800000 ? (v & 0x3fffff) << 2 & 0xfff : 0;
}

/* The TA started again on a scene it holds (after a context load, say):
 * the next block is shared again, as after an overflow's resume */
void pb_restart(struct mbxemu *e)
{
	e->pbw.first_done = 0;
}

/*
 * n bytes from macro tile m's page; 0 if the free list is empty. What
 * doesn't fit goes at the start of a new page, the rest of the old one
 * left unused (a 32-word block with 16 words left)
 */
static uint32_t mt_alloc(struct mbxemu *e, uint32_t m, uint32_t n)
{
	struct pbw *w = &e->pbw;
	uint32_t a;

	if (!w->mt_page[m] || w->mt_bump[m] + n > 4096) {
		if (!(a = page_alloc(e)))
			return 0;
		mt_page_taken(e, m, a);
		w->mt_page[m] = a;
		w->mt_bump[m] = 0;
	}
	a = w->mt_page[m] + w->mt_bump[m];
	w->mt_bump[m] += n;
	return a;
}

/* The block's words for the vertices gathered, into out; their count */
static int block_words(const struct mbxemu *e, uint32_t *out)
{
	const struct pbw *w = &e->pbw;
	const struct rstate *s = &e->cur;
	int n = 0, k, u, t, units = w->units;

	out[n++] = s->at;
	for (u = 0; u < units; u++)
		for (k = 0; k < 3; k++)
			out[n++] = s->tex[u][k];
	out[n++] = s->st | w->fan << 6;	/* the TA marks a fan's blocks */
	for (k = 0; k < w->nv; k++) {
		out[n++] = f2u(w->v[k].x);
		out[n++] = f2u(w->v[k].y);
	}
	for (k = 0; k < w->nv; k++)
		out[n++] = f2u(w->v[k].z);
	for (k = 0; k < w->nv; k++)
		out[n++] = f2u(w->v[k].rhw);
	for (k = 0; k < w->nv; k++) {
		const float *c = w->v[k].col;
		uint32_t argb = 0;

		for (t = 0; t < 4; t++)
			argb |= (uint32_t)(c[t] * 255 + 0.5f) << (t == 3 ? 24 : 16 - 8 * t);
		out[n++] = argb;
		for (u = 0; u < units; u++) {
			out[n++] = f2u(w->v[k].tc[u][0]);
			out[n++] = f2u(w->v[k].tc[u][1]);
		}
	}
	return n;
}

/* Append entry to tile (x, y)'s list in macro tile m; -1, with nothing
 * changed, if a new list block is wanted and there's no page for it */
static int list_add(struct mbxemu *e, uint32_t m, uint32_t tile, uint32_t entry)
{
	struct pbw *w = &e->pbw;
	uint32_t a;

	if (!w->list_at[tile] || w->list_n[tile] == LIST_WORDS - 1) {
		if (!(a = mt_alloc(e, m, LIST_WORDS * 4)))
			return -1;
		if (w->list_at[tile])	/* the full list links on */
			dev_write32(e, w->list_at[tile] + (LIST_WORDS - 1) * 4, LINK | a);
		else
			w->list_head[tile] = a;
		w->list_at[tile] = a;
		w->list_n[tile] = 0;
	}
	dev_write32(e, w->list_at[tile] + w->list_n[tile] * 4, entry);
	w->list_n[tile]++;
	dev_write32(e, w->list_at[tile] + w->list_n[tile] * 4, LIST_END);
	return 0;
}

/*
 * Bin the pending block, from where it got to: its shared copy first if
 * it is the first since the TA started (on the page the first blocks
 * share, a new one when that is full), then macro tile by macro tile,
 * tile by tile, its copy in each macro tile's page (unless shared) and a
 * pointer in each tile's list. -1 if the free list ran out on the way:
 * the job keeps its place for pb_resume(); with mt_end, 1 once the macro
 * tile it is in is done (a close's end).
 */
static int job_run(struct mbxemu *e, int mt_end)
{
	struct pbw *w = &e->pbw;
	struct pbjob *j = &w->job;
	uint32_t x, y, k, mask, a;
	int t;

	if (!j->placed) {
		if (j->first) {
			if (!w->gpage || w->gbump + j->n * 4 > 4096) {
				if (!(a = page_alloc(e)))
					return -1;
				w->gpage = a;
				w->gbump = 0;
			}
			j->shared = w->gpage + w->gbump;
			w->gbump += j->n * 4;
			for (k = 0; k < j->n; k++)
				dev_write32(e, j->shared + 4 * k, j->words[k]);
		}
		j->placed = 1;
		j->m = j->i = 0;
		j->at = j->shared;
	}
	for (; j->m < w->nmx * w->nmy; j->m++, j->i = 0, j->at = j->shared) {
		uint32_t mx0 = j->m % w->nmx * w->mx, my0 = j->m / w->nmx * w->my;

		if (mx0 > j->x1 || my0 > j->y1 || mx0 + w->mx <= j->x0 || my0 + w->my <= j->y0)
			continue;
		for (; j->i < w->mx * w->my; j->i++) {
			x = mx0 + j->i % w->mx;
			y = my0 + j->i / w->mx;
			if (x >= w->tx || y >= w->ty)
				continue;
			mask = 0;
			for (t = 0; t < j->ntri; t++)
				if (x >= j->bx0[t] && x <= j->bx1[t] && y >= j->by0[t] && y <= j->by1[t])
					mask |= 1u << (30 - j->tri[t]);
			if (!mask)
				continue;
			if (!j->at) {
				/* a later block: copied into this macro tile's page first */
				if (!(j->at = mt_alloc(e, j->m, j->n * 4)))
					return -1;
				for (k = 0; k < j->n; k++)
					dev_write32(e, j->at + 4 * k, j->words[k]);
			}
			if (list_add(e, j->m, y * w->tx + x, mask | j->field << 21 | j->at >> 2))
				return -1;
		}
		if (mt_end && j->m + 1 < w->nmx * w->nmy) {
			j->m++;
			j->i = 0;
			j->at = j->shared;
			return 1;
		}
	}
	j->pending = 0;
	return 0;
}

/* Out of pages: TA_OFLOW, and the TA takes no more until resumed */
static void stall(struct mbxemu *e)
{
	e->pbw.stalled = 1;
	emu_trace(e, "parameter buffer: out of pages, TA_OFLOW");
	emu_raise(e, I_TA_OFLOW);
}

/* TA_START while stalled (the daemon's resume, 0x804 then 0x800): the
 * block in hand is binned on, from the free list as it now is; the next
 * new block is shared (the N810 put it after the scene's first). 0 if the
 * TA takes input again */
int pb_resume(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;

	w->first_done = 0;
	if (w->job.pending && job_run(e, 0)) {
		stall(e);
		return -1;
	}
	w->stalled = 0;
	return 0;
}

/*
 * A close (a partial render) while stalled: the TA finishes the
 * macro tile it ran out in, with the list's last page, and stops there
 * (three closes each end with the macro tile the last page went to). The region array is then written (ta_end_scene())
 */
void pb_close(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;

	if (!w->job.pending)
		return;
	w->closing = 1;
	job_run(e, 1);
	w->closing = 0;
}

/* and after it the tiles' lists start over (software clears the tail
 * pointers); the shared page and each macro tile's next free address
 * carry on */
void pb_closed(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;

	memset(w->list_at, 0, sizeof(w->list_at));
	memset(w->list_head, 0, sizeof(w->list_head));
	memset(w->list_n, 0, sizeof(w->list_n));
}

static void block_flush(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;
	struct pbjob *j = &w->job;
	int k, t;

	if (!w->on || !w->ntri) {
		w->nv = w->ntri = 0;
		return;
	}
	if (j->pending) {
		emu_log(e, "parameter buffer: a block while the TA is stalled, dropped");
		w->nv = w->ntri = 0;
		return;
	}
	memset(j, 0, sizeof(*j));
	j->n = (uint32_t)block_words(e, j->words);
	j->x0 = j->y0 = ~0u;
	/* each triangle's bounding box in tiles, clamped to the screen */
	for (t = 0; t < w->ntri; t++) {
		float fx0 = 1e30f, fy0 = 1e30f, fx1 = -1e30f, fy1 = -1e30f;

		j->tri[t] = w->tri[t];
		for (k = 0; k < 3; k++) {
			const struct vtx *v = &w->v[w->tri[t] + k];

			fx0 = fminf(fx0, v->x);
			fy0 = fminf(fy0, v->y);
			fx1 = fmaxf(fx1, v->x);
			fy1 = fmaxf(fy1, v->y);
		}
		if (fx1 < 0 || fy1 < 0 || fx0 >= w->tx * TILE || fy0 >= w->ty * TILE) {
			j->bx0[t] = 1;
			j->bx1[t] = 0;	/* off the screen: in no tile */
			continue;
		}
		j->bx0[t] = fx0 < 0 ? 0 : (uint32_t)fx0 / TILE;
		j->by0[t] = fy0 < 0 ? 0 : (uint32_t)fy0 / TILE;
		j->bx1[t] = (uint32_t)fx1 / TILE < w->tx ? (uint32_t)fx1 / TILE : w->tx - 1;
		j->by1[t] = (uint32_t)fy1 / TILE < w->ty ? (uint32_t)fy1 / TILE : w->ty - 1;
		j->x0 = j->bx0[t] < j->x0 ? j->bx0[t] : j->x0;
		j->y0 = j->by0[t] < j->y0 ? j->by0[t] : j->y0;
		j->x1 = j->bx1[t] > j->x1 ? j->bx1[t] : j->x1;
		j->y1 = j->by1[t] > j->y1 ? j->by1[t] : j->y1;
	}
	j->ntri = w->ntri;
	j->field = (uint32_t)w->nv + 1 + 4 * (uint32_t)w->units;
	w->nv = w->ntri = 0;
	if (j->x0 == ~0u)
		return;
	j->first = !w->first_done;
	w->first_done = 1;
	j->pending = 1;
	if (job_run(e, 0))
		stall(e);
}

/* A fan's block: rim vertices r0, r1, the centre, r2, r3 (as many as
 * there are), a strip's triangles */
static void fan_flush(struct mbxemu *e)
{
	struct pbw *w = &e->pbw;
	int k;

	if (w->nrim < 2)
		return;
	w->fan = 1;
	w->nv = 0;
	for (k = 0; k < w->nrim; k++) {
		if (k == 2)
			w->v[w->nv++] = w->centre;
		w->v[w->nv++] = w->rim[k];
	}
	if (w->nrim == 2)
		w->v[w->nv++] = w->centre;
	for (w->ntri = 0; w->ntri < w->nv - 2; w->ntri++)
		w->tri[w->ntri] = w->ntri;
	block_flush(e);
	w->fan = 0;
}

/* A point's or a line segment's quad: four vertices, a strip's two
 * triangles, a block of its own */
static void quad_flush(struct mbxemu *e, const struct vtx *v)
{
	struct pbw *w = &e->pbw;
	int k;

	for (k = 0; k < 4; k++)
		w->v[k] = v[k];
	w->nv = 4;
	w->ntri = 2;
	w->tri[0] = 0;
	w->tri[1] = 1;
	block_flush(e);
}

/*
 * A vertex of a draw (projected). Lists make a triangle of every three;
 * strips one of every vertex from the third, six vertices a block, the
 * next block starting with the last two. A fan goes out as strips: rim
 * vertices r0 r1, the centre, r2 r3 (three triangles), the next block
 * starting from r3. A point is a square of the point size, (x, y) +- h,
 * moved by (1/32, -1/32) (measured at size 4 only); a line segment's
 * ends are moved by +-h across its major axis (the first vertex to the
 * left of its direction, y down), a block each.
 */
void pb_vertex(struct mbxemu *e, const struct vtx *v)
{
	struct pbw *w = &e->pbw;
	uint32_t prim = e->prim_word >> 28, st = e->cur.st >> 16 & 7;
	float h = e->half_size;
	struct vtx q[4];
	int k;

	if (!w->on)
		return;
	w->units = st == 5 ? 2 : st == 2 || st == 4 ? 1 : 0;
	switch (prim) {
	case 0x7:
	case 0x4:
		if (w->nv == 6 && prim == 0x4) {
			w->v[0] = w->v[4];
			w->v[1] = w->v[5];
			w->nv = 2;
		}
		w->v[w->nv++] = *v;
		w->obj_v++;
		if (prim == 0x7 ? w->nv % 3 == 0 : w->obj_v >= 3)
			w->tri[w->ntri++] = w->nv - 3;
		if (w->nv == 6) {
			block_flush(e);
			if (prim == 0x4)
				w->nv = 6;	/* kept for the next block's first two */
		}
		return;
	case 0x5:
		if (!w->obj_v++) {
			w->centre = *v;
			return;
		}
		w->rim[w->nrim++] = *v;
		if (w->nrim == 4) {
			fan_flush(e);
			w->rim[0] = w->rim[3];
			w->nrim = 1;
		}
		return;
	case 0xe:
		for (k = 0; k < 4; k++) {
			q[k] = *v;
			q[k].x += (k & 1 ? h : -h) + 1.0f / 32;
			q[k].y += (k & 2 ? h : -h) - 1.0f / 32;
		}
		quad_flush(e, q);
		return;
	case 0x6:
		if (w->obj_v++) {
			const struct vtx *a = &w->rim[0];
			float dx = v->x - a->x, dy = v->y - a->y;
			/* across the major axis, to the left of the direction */
			float nx = fabsf(dx) >= fabsf(dy) ? 0 : dy > 0 ? -h : h;
			float ny = fabsf(dx) >= fabsf(dy) ? (dx > 0 ? h : -h) : 0;

			for (k = 0; k < 4; k++) {
				q[k] = k < 2 ? *a : *v;
				q[k].x += k & 1 ? -nx : nx;
				q[k].y += k & 1 ? -ny : ny;
			}
			quad_flush(e, q);
		}
		w->rim[0] = *v;
		return;
	}
}

/* The draw ends (the control port's 1), or a line strip restarts (0) */
void pb_object_end(struct mbxemu *e)
{
	if ((e->prim_word >> 28) == 0x5)
		fan_flush(e);
	else
		block_flush(e);
	e->pbw.nv = 0;
	e->pbw.nrim = 0;
	e->pbw.obj_v = 0;
}
