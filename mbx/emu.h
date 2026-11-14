/* SPDX-License-Identifier: MIT */
/*
 * mbxemu's internals: the device state shared by core.c (registers,
 * interrupts, MMU, contexts), ta.c (the TA stream), vgp.c (the vertex
 * processor), render.c (the 3D core), texture.c (the TSP's textures and
 * combiners) and twod.c (the 2D core).
 */
#ifndef MBXEMU_EMU_H
#define MBXEMU_EMU_H

#include <stddef.h>
#include <stdint.h>

#include "mbxemu.h"

/* --- registers (byte offsets; names as software uses them) --- */
#define R_SOFT_RESET	0x080
#define R_INT_STATUS	0x12c	/* a write sets bits: the doorbell */
#define R_INT_ENABLE	0x130
#define R_INT_CLEAR	0x134
#define R_REGIONS	0x608	/* the region array a render walks */
#define R_RENDER_CTL	0x610	/* Z store (bit 0), load (bit 1), stride from bit 8; 16-17 on chip */
#define R_SAMPLING	0x61c	/* 0x00020007: bit 0 set-up FPU pixel centre, 1 TSP pixel centre,
				 * 2 TSP texel centre (the half texel); 16-17 the depth
				 * plane's reference point */
#define R_FOG_COLOUR	0x63c	/* ARGB */
#define R_FORMAT	0x650	/* 0xe ARGB8888, 9 RGB565 */
#define R_WIDTH		0x654	/* (width - 1) << 16 */
#define R_HEIGHT	0x658
#define R_ADDR		0x65c	/* the colour buffer */
#define R_STRIDE	0x660	/* in 32-bit words */
#define R_ZBUF		0x674	/* the external Z buffer */
#define R_KICK		0x680
#define R_6DC		0x6dc	/* 0x3f800001: taken for the depth a tile starts with */
#define R_HW_CONTEXT	0x6ec	/* the TA context the render reads */
#define R_DAMAGE	0x7f0	/* not a register (the damage rectangle, for host software) */
#define R_TA_START	0x800
#define R_TA_RESUME	0x804
#define R_TA_CLOSE	0x808
#define R_TA_CONTEXT_ID	0x810
#define R_TA_CTX_LOAD	0x814
#define R_TA_CTX_STORE	0x818
#define R_TA_CTX_RESET	0x81c
#define R_TA_CTX_BASE	0x820
#define R_FL_TABLE	0x824
#define R_FL_FIRST	0x828
#define R_FL_LAST	0x82c
#define R_FL_BUILD	0x838
#define R_PB_BASE	0x83c	/* the parameter buffer: the background object */
#define R_TA_TAILS	0x840
#define R_TA_REGIONS	0x844
#define R_TA_WIDTH	0x84c
#define R_TA_HEIGHT	0x850
#define R_TA_CONFIG	0x85c
#define R_TILES		0x860
#define R_EVM_CTX_BASE	0x864
#define R_ISP_SIG	0xcc4
#define R_CORE_ID	0xf00
#define R_REVISION	0xf10
#define R_MMU_TABLE(i)	(0x1000 + 4 * (i))
#define R_MMU_CTRL	0x1020	/* bit 0 enable, 16 enabled, 8 TLB load, 24 busy */
#define R_MMU_TLB_PAGE	0x1024
#define R_MMU_TLB_PHYS	0x1028

/* INT_STATUS bits (0 and 3: seen on the N810 with the kernel driver, README assumption 32) */
#define I_B0		0x1
#define I_RENDER	0x4
#define I_ISP		0x8
#define I_TA_DONE	0x10
#define I_TA_OFLOW	0x20
#define I_EVM_DALLOC	0x40
#define I_TA_CONTEXT	0x100
#define I_TA_STREAM	0x200
#define I_2D		0x400
#define I_MASTER	0x8000

/* TA_CONFIG bits */
#define TAC_PARTIAL	0x8	/* a partial render runs: it stores depth */
#define TAC_END_IS_END	0x80	/* clear: the end word stops the TA mid-scene */
#define TAC_ZLOAD	0x40000

/* A sections word needs bit 17 for the TA to take it (README assumption 30) */
#define SEC_BIT17	0x20000
#define TA_FIFO_WORDS	59

#define DEV_SPACE	(32u << 20)	/* the MMU's device address space */
#define TILE		16

/* --- the TA's state blocks, as an object header sets them --- */
struct rstate {
	uint32_t st;		/* ISP/TSP state */
	uint32_t at;		/* alpha test, environment colour */
	uint32_t tex[2][3];	/* texture words A, B, C a unit */
	uint32_t w4, layout, w6;
	float vp[4];		/* x scale, x offset, y offset, y scale */
	float dz[3];		/* depth scale, offset, the unit of the offset (1e-5) */
	uint32_t stream[3];	/* VGP stream-out: address (ignored), selects, enable */
};

/* A vertex after the VGP, in surface pixels */
struct vtx {
	float x, y, z;		/* z: depth, after the depth transform */
	float rhw;		/* 1 / w, for perspective-correct attributes */
	float col[4];		/* the base colour, RGBA (o1 is BGRA) */
	float off[4];		/* the offset colour RGB, the fog factor in [3] */
	float tc[2][2];		/* texture coordinates, units 0 and 1 */
};

/* A triangle kept for the render */
struct prim {
	struct vtx v[3];
	float fcol[4], foff[4];	/* the colours flat shading takes: the last vertex sent's */
	uint32_t pw;		/* the primitive word */
	int state;		/* index of its state block in the scene */
	int x0, y0, x1, y1;	/* bounding box in pixels, ends exclusive */
};

/* A vertex out of the VGP, before clipping: clip space */
#define NCLIP 11		/* w > 0, front and rear (o7.x, o7.y), eight user planes */
struct cvtx {
	float pos[4];		/* o0 */
	float o[4][4];		/* o1 colour (BGRA), o2 offset, o3 and o4 texture coordinates */
	float cd[NCLIP];	/* distances to the clip planes in force; < 0 is out */
};

/* A scene: what the TA has binned for a render (our "parameter buffer") */
struct scene {
	int used;		/* the slot holds a scene */
	int complete;		/* its end word came: the next render consumes it */
	int closed;		/* a close (0x808) came: a partial render may take it */
	int has_nowrite_clear;	/* a screen-space object with no colour and no depth write: it only gives every tile a list (the render loads the depth) */
	int has_clear;		/* a depth-only clear object (screen space, depth written, no colour) came */
	uint32_t regions, tails, config, width, height;	/* the TA's registers at its start */
	struct rstate *states;
	int nstates, cstates;
	struct prim *prims;
	int nprims, cprims;
	struct rstate ta_state;	/* the TA's state blocks while the scene is stored */
	uint32_t key;		/* a stored scene: its TA context address (0x820) */
	struct scene *next;
};

enum ta_mode {
	TA_CMD,			/* a command word */
	TA_HDR,			/* an object header's blocks */
	TA_SEC_WORD1,		/* a VGP section's flags and range */
	TA_SEC_CODE_HDR,	/* its code word */
	TA_CODE,		/* instructions */
	TA_CONST,		/* constants */
	TA_PRIM_SIZE,		/* half the point size or line width */
	TA_PRIM_SECTIONS,	/* the sections word */
	TA_VERTS,		/* vertices, until a control word */
	TA_ERROR,		/* lost: everything until the next TA start */
	TA_HUNG,		/* the TA takes nothing (a sections word without bit 17): its words pile up in the FIFO */
};

struct vgp {
	uint32_t code[256][2];	/* instructions: high word, low word */
	float k[192][4];	/* constants */
	struct {
		int loaded;
		uint32_t flags;	/* 0-3 loop count - 1, 4-7 constant window, 8-11 its end */
		int first, last;
	} sec[16];		/* by bit number: 0x4 is 2, 0x4000 is 14 */
};

/* The VGP's registers while it runs a vertex */
struct vgp_regs {
	float v[8][4];		/* inputs */
	float r[14][4];		/* temporaries */
	float o[8][4];		/* outputs */
	int a0;
};

struct twod {
	uint32_t dfmt, dstride, daddr;
	uint32_t sfmt, sstride, saddr, sx, sy;
	int have_src;
	uint32_t xs, ys;	/* source per destination pixel, 5.5 fixed point */
	int key_on, key_src;
	uint32_t key, key_mask;
	int alpha_on, alpha_pp;	/* global alpha, or source alpha over */
	uint32_t alpha;
	int nclip;
	uint32_t clip[4][2];
	uint32_t blit, colour;
	int state, want, got;	/* the words of the command in progress */
	uint32_t buf[8];
};

/* A device page cache for one render or blit: textures read once */
#define PCACHE 64
struct pcache {
	uint32_t page[PCACHE];	/* device page + 1, 0 empty */
	uint8_t data[PCACHE][4096];
};

/* The parameter buffer the TA writes (pb.c): tiles and macro tiles of the
 * largest render it models */
#define PB_TILES	4096
#define PB_MTS		256
/* A block being binned: where it got to, to go on after an overflow */
struct pbjob {
	int pending;
	int first;			/* the first block since the TA started: shared */
	int placed;			/* its shared copy is written (or not wanted) */
	uint32_t words[96], n, field, shared;
	uint32_t bx0[4], by0[4], bx1[4], by1[4], x0, y0, x1, y1;
	int ntri, tri[4];
	uint32_t m, i, at;		/* macro tile, tile in it, the block's copy there */
};

struct pbw {
	int on;				/* a scene the writer models */
	uint32_t cur, tail;		/* the free list: the page last handed out (its
					 * entry names the next), the last free page */
	int stalled;			/* out of pages: TA_OFLOW, input queued */
	int fl_ok;			/* a free list was built */
	int closing;			/* a close: the list's last page may go */
	uint32_t tx, ty, mx, my, nmx, nmy;
	int first_done;			/* the first block since the TA started is out */
	uint32_t gpage, gbump;		/* the page the first blocks share */
	uint32_t taken;			/* pages handed out since the scene started (a trace) */
	struct pbjob job;
	uint32_t mt_page[PB_MTS], mt_bump[PB_MTS];
#define PB_MT_PAGES	768	/* the TA can hand out at most the 768 pages of the parameter buffer: a macro tile may hold them all */
	uint16_t mt_pages[PB_MTS][PB_MT_PAGES];	/* each macro tile's pages, in order */
	uint16_t mt_npages[PB_MTS];
	uint32_t list_at[PB_TILES];	/* each tile's current list block */
	uint32_t list_head[PB_TILES];	/* and its first (the region array's) */
	uint8_t list_n[PB_TILES];	/* and its entries */
	struct vtx v[6];		/* the block being gathered */
	int nv, ntri, tri[4], units, obj_v;
	struct vtx centre, rim[4];	/* a fan's; rim[0] a line's last vertex */
	int nrim, fan;
};

/*
 * What a client must have done for the chip to work (README assumption 31): each is a
 * check at TA start or at the render kick, counted and logged once a scene. In strict mode
 * (the default) a scene with any of them, or a background object that is not whole,
 * comes out as the chip's does: white garbage in the tiles that hold objects (and, for a
 * bad background object, in every tile's start).
 */
#define V_FL_OVERLAP	(1u << 0)	/* the free list's first page is not above the buffer's base page */
#define V_FL_REGS	(1u << 1)	/* 0x6d8 is not 0x09000000 or 0xce0 is not 1 */
#define V_PB_BASE	(1u << 2)	/* 0x83c, the parameter buffer's base, is not 0 */
#define V_TAILS_ALIGN	(1u << 3)	/* 0x840 is not 64 KB aligned */
#define V_CTX		(1u << 4)	/* 0x820 or 0x864 (the TA and EVM contexts) is not set */
#define V_REGION_INIT	(1u << 5)	/* the region array was not initialised (0x22000000 | origin) */
#define V_REGION_B30	(1u << 6)	/* the render's region entries lack bit 30 */
#define V_BG		(1u << 7)	/* the background object is not a whole object */
#define V_LIST		(1u << 8)	/* a tile's list in memory is damaged (an empty entry, no end word) */
#define V_FL_STALE	(1u << 9)	/* a partial render after a partial render, the free list not rebuilt */
#define V_RENDER_EARLY	(1u << 10)	/* a render kicked while the scene is still being streamed */
#define V_STRAY		(1u << 11)	/* a write through a page no table entry maps (no scratch page) */
#define V_REGION_ENTRY	(1u << 12)	/* a region entry with neither a list address nor bit 25 */
/*
 * Soft checks: from the stock stack or GLES's choices, not confirmed on the device (README
 * assumption 31). They are counted and logged; they do not turn the picture to garbage.
 */
#define V_ALIGN_SOFT	(1u << 13)	/* 0x820/0x824 not 16 KB, 0x844/0x864 not 4 KB aligned */
#define V_GRID		(1u << 14)	/* 0x85c's grid and 0x84c/0x850's size disagree */
#define V_FMT		(1u << 15)	/* the render's format, address or stride and the background object's disagree */
#define V_FL_CHAIN	(1u << 16)	/* the free list's registers or table do not make a chain */
#define V_TA_BASE	(1u << 17)	/* 0x85c lacks the base bits 0xe1 */
#define V_REGS		(1u << 18)	/* a render or TA register of the set was not written since the last kick/start */
#define V_WRITE_OUT	(1u << 19)	/* the chip wrote outside the target, the buffer, the tails, the regions, the contexts */
#define V_TLB		(1u << 23)	/* a page the chip used was not loaded into the TLB (0x1024, 0x1028, 0x1020 bit 8) */
#define V_MMU_SEQ	(1u << 24)	/* the MMU was enabled without the disable, wait, enable, wait handshake */
#define V_TILES		(1u << 22)	/* 0x860 does not match the tile grid in 0x85c: the deallocation frees the wrong pages */
#define V_PB_FILL	(1u << 21)	/* the parameter buffer was not filled with 0xf0000000 end words before the build */
#define V_DEPTH_CLEAR	(1u << 20)	/* the scene has no depth-only clear: the tile's depth starts as NaN */
#define V_STATE25	(1u << 25)	/* a draw that writes colour with bit 25 of its state word clear: the colour path differs on the chip (ANDed with the target) */
#define V_TEXMODE	(1u << 26)	/* state bits 16-18 are 1, 3, 6 or 7: on the chip a textured draw with them draws nothing (1) or hangs the job */
#define V_SOFT		(V_STATE25 | V_TLB | V_MMU_SEQ | V_TILES | V_PB_FILL | V_DEPTH_CLEAR | V_ALIGN_SOFT | V_GRID | V_FMT | V_FL_CHAIN | V_TA_BASE | V_REGS | V_WRITE_OUT)

struct mbxemu {
	struct mbxemu_host host;
	int strict;			/* violations have the chip's consequences (default) */
	uint32_t viol;			/* this scene's violations */
	int fl_stale;			/* a partial render has run since the free list was built */
	int tiles_bad;			/* the last kick had 0x860 not matching 0x85c: its pages are not given back */
	uint32_t written[0x4000 / 4 / 32];	/* registers written since they were last checked (render: the kick; TA: the start) */
	int watch;			/* the TA or the render is running: bus writes are checked */
	uint32_t viol_ever;
	int trace;
	uint32_t regs[0x4000 / 4];
	uint32_t status, enable;	/* INT_STATUS's low bits, INT_ENABLE */
	int irq_level;

	/* MMU */
	uint32_t tlb[DEV_SPACE >> 12];	/* physical page + 1, 0 not loaded */
	uint8_t tlb_sw[(DEV_SPACE >> 12) / 8];	/* pages loaded by software (0x1024/0x1028, bit 8), not by a walk */
	int mmu_off_seen, mmu_hand;	/* 0x1020 written 0, then 1: the enable handshake (README assumption 31) */

	/* the TA */
	struct scene ctx[2];		/* the two hardware contexts */
	struct scene *stored;		/* scenes stored to TA context memory */
	enum ta_mode mode;
	unsigned int fifo_fill;		/* words waiting in the TA's input FIFO: only a hung TA has any */
	struct rstate cur;		/* the state blocks in force */
	uint32_t hdr_mask;
	int hdr_bit, hdr_left, hdr_word;
	int sec_bit, code_at, code_end, code_half;
	int const_at, const_end, const_lane;
	uint32_t prim_word, sections;
	float half_size;
	uint32_t vin[64];
	int vin_n, vsize;
	int scene_state;		/* the current state block's index, -1 none yet */
	struct cvtx asm_v[2];		/* primitive assembly: the last two vertices */
	int asm_n;			/* vertices since the object or a restart */
	struct cvtx first_v;		/* a fan's centre */
	int no_scene_warned;

	/* VGP stream-out, as measured (README, assumption 23) */
	int so_on;			/* this object writes its vertices out */
	int so_side;			/* 0 X, 1 Y: flips after each object that streamed
					 * and at each render */
	uint32_t so_y;			/* Y's place next */
	uint32_t so_at;			/* where the next vertex goes */

	struct pbw pbw;
	uint32_t (*tq)[2];		/* TA input kept while stalled: lane (4 control), word */
	size_t tqn, tqcap;
	struct vgp vgp;
	struct twod twod;
	struct pcache *pc;

	uint32_t isp_sig;
	struct mbxemu_stats stats;
};

/* core.c */
void emu_log(struct mbxemu *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void emu_violation(struct mbxemu *e, uint32_t bit, const char *what);
void emu_check_write(struct mbxemu *e, uint32_t dev, uint32_t len);	/* core.c: the write watch */
#define emu_trace(e, ...) do { if ((e)->trace) emu_log(e, __VA_ARGS__); } while (0)
void emu_raise(struct mbxemu *e, uint32_t bits);
/* Device memory through the MMU; out of range or unmapped reads 0 */
void dev_read(struct mbxemu *e, uint32_t dev, void *buf, uint32_t len);
void dev_write(struct mbxemu *e, uint32_t dev, const void *buf, uint32_t len);
uint32_t dev_read32(struct mbxemu *e, uint32_t dev);
void pb_ctx_store(struct mbxemu *e, uint32_t base);
void pb_ctx_load(struct mbxemu *e, uint32_t base);
void dev_write32(struct mbxemu *e, uint32_t dev, uint32_t v);
/* Reads through the page cache while it is on (a render, a blit) */
void pcache_begin(struct mbxemu *e);
void pcache_end(struct mbxemu *e);
void scene_free(struct scene *s);
void scene_clear_prims(struct scene *s);
struct scene *ta_scene(struct mbxemu *e);

/* ta.c */
void ta_word(struct mbxemu *e, uint32_t lane, uint32_t v);
void ta_ctl(struct mbxemu *e, uint32_t v);
void ta_start(struct mbxemu *e);
void ta_reset(struct mbxemu *e);
void ta_end_scene(struct mbxemu *e, int close);
void tile_grid(uint32_t config, uint32_t *tx, uint32_t *ty, uint32_t *mx, uint32_t *my);

/* vgp.c */
void vgp_run(struct mbxemu *e, struct vgp_regs *r, uint32_t sections);
void so_reset(struct mbxemu *e);

/* pb.c */
void pb_fl_built(struct mbxemu *e, int built);
void pb_scene_start(struct mbxemu *e, uint32_t config);
void pb_vertex(struct mbxemu *e, const struct vtx *v);
void pb_object_end(struct mbxemu *e);
void pb_restart(struct mbxemu *e);
int pb_resume(struct mbxemu *e);
void pb_close(struct mbxemu *e);
void pb_closed(struct mbxemu *e);
void pb_render(struct mbxemu *e);
void pb_render_leak(struct mbxemu *e);	/* the pages a render would give back are lost */
void so_render(struct mbxemu *e);
void vgp_exec(const struct vgp *g, struct vgp_regs *r, int first, int last, int window);

/* render.c */
void render_kick(struct mbxemu *e);

/* rcptab.c: the reciprocal ROM's offset from trunc16(1/q) for q's 16-bit significand */
int rcp_offset(int m);

/* texture.c */
struct texunit {
	uint32_t a, b, c;
	uint32_t addr, fmt, stride;	/* stride: texels a row, 0 twiddled */
	int lw, lh;
	int clamp_s, clamp_t, flip_s, flip_t, bilinear, mip, trilinear, dot3;
	int maxlev;		/* word A bits 9-12: the last mip level taken */
	int pvrtc;		/* word A bit 17: PVRTC, 2 bpp (format 0) or 4 bpp (format 6) */
	float half;			/* the texel centre offset: 0.5, or 0 (0x61c bit 2 clear) */
};
void tex_decode(struct texunit *t, const uint32_t w[3]);
/* RGBA 0..1 at (u, v) in texture coordinates, lod the level of detail */
void tex_sample(struct mbxemu *e, const struct texunit *t, float u, float v, float lod, float out[4]);
/* One texel of level 0 at integer coordinates (x, y), RGBA 0..1 */
void tex_fetch(struct mbxemu *e, const struct texunit *t, int level, int x, int y, float out[4]);
/* The combiner of word C: prev (the unit before), prim, the constant, the texel */
void tex_combine(uint32_t c, int dot3, const float prev[4], const float prim[4],
		 const float konst[4], const float texel[4], float out[4]);
uint32_t twiddle(uint32_t u, uint32_t v, int lw, int lh);

/* twod.c */
void twod_word(struct mbxemu *e, uint32_t v);
void twod_reset(struct mbxemu *e);

static inline float u2f(uint32_t u)
{
	union { uint32_t u; float f; } x = { u };

	return x.f;
}

static inline uint32_t f2u(float f)
{
	union { float f; uint32_t u; } x = { f };

	return x.u;
}

static inline float clamp01(float x)
{
	return x < 0 ? 0 : x > 1 ? 1 : x;
}

#endif
