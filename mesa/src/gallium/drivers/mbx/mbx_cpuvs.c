/* SPDX-License-Identifier: MIT */
/*
 * The vertex program on the CPU, for the grid mode (README assumption 48): where a vertex lands on the screen decides how
 * finely a triangle is cut. The interpreter is the emulator's (mbx/vgp.c, copied beside this file by
 * mesa/scripts/prepare-mesa.sh); it is kept to this file because its header names things Mesa's do.
 */
#include <string.h>

#include "emu.h"
#include "mbx_program.h"

void mbx_cpuvs_run(const uint32_t code[][2], unsigned ninsn, const float k[][4], unsigned nk, const float in[8][4],
		   float pos[4])
{
	static struct vgp g;		/* a constant file of 192 vec4s; the driver is single-threaded per context */
	struct vgp_regs r;

	memset(&r, 0, sizeof(r));
	memcpy(r.v, in, sizeof(r.v));
	memcpy(g.code, code, ninsn * sizeof(g.code[0]));
	memcpy(g.k, k, (nk > 192 ? 192 : nk) * sizeof(g.k[0]));
	vgp_exec(&g, &r, 0, (int)ninsn - 1, 0);
	memcpy(pos, r.o[0], 4 * sizeof(float));
}

/*
 * Whether the colour outputs of the vertices (o1, and o2 without its fog factor) leave 0..1: the chip clamps them a vertex. The program and
 * the constants are set up once for the draw (on the unit a vertex's run is the cost: the set-up of 3 KB of constants was most of it).
 */
static struct vgp g_oor;

void mbx_cpuvs_oor_begin(const uint32_t code[][2], unsigned ninsn, const float k[][4], unsigned nk)
{
	memcpy(g_oor.code, code, ninsn * sizeof(g_oor.code[0]));
	memcpy(g_oor.k, k, (nk > 192 ? 192 : nk) * sizeof(g_oor.k[0]));
}

bool mbx_cpuvs_oor_vertex(unsigned ninsn, const float in[8][4])
{
	struct vgp_regs r;

	memset(&r, 0, sizeof(r));
	memcpy(r.v, in, sizeof(r.v));
	vgp_exec(&g_oor, &r, 0, (int)ninsn - 1, 0);
	for (unsigned c = 0; c < 4; c++)
		if (r.o[1][c] < -0.002f || r.o[1][c] > 1.002f || (c < 3 && (r.o[2][c] < -0.002f || r.o[2][c] > 1.002f)))
			return true;
	return false;
}

/* The vertex program's outputs (o0 the position, o1 and o2 the colours, o3 and o4 the texture coordinates of units 0 and 1) for one vertex */
void mbx_cpuvs_outputs(const uint32_t code[][2], unsigned ninsn, const float k[][4], unsigned nk, const float in[8][4], float o[8][4])
{
	static struct vgp g;
	struct vgp_regs r;

	memset(&r, 0, sizeof(r));
	memcpy(r.v, in, sizeof(r.v));
	memcpy(g.code, code, ninsn * sizeof(g.code[0]));
	memcpy(g.k, k, (nk > 192 ? 192 : nk) * sizeof(g.k[0]));
	vgp_exec(&g, &r, 0, (int)ninsn - 1, 0);
	memcpy(o, r.o, sizeof(r.o));
}
