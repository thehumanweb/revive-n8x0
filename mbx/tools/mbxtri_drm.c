// SPDX-License-Identifier: MIT
/*
 * mbxtri-drm: mbxtri through the kernel's render node. Buffers by
 * DRM_IOCTL_MBX_BO_CREATE/INFO, the same stream (tri_frame.h) in one
 * DRM_IOCTL_MBX_SUBMIT, the picture read back through the buffer's mmap.
 *
 *   mbxtri-drm [-a degrees] [-d /dev/dri/renderD129] [-o out.ppm] [-D] [-P file] [-E] [-G] [-X mode] [-W raw.bin] [-F fill] [-Z] [-K clear_state] [-A alpha] [-B clear_alpha] [-R prefix] [-C r_ctl] [-M r_sampling] [-S state_word]
 *
 * -D dumps what the job left in the region array, the tail pointers and the
 * background-object buffer (nonzero words), to see where the TA binned the triangle.
 * -P file writes the parameter buffer's first 8 pages after the table's 16 KB
 * (DRM_IOCTL_MBX_PB_DUMP, root only) to file, raw, with the table's first 64 bytes first.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/mbx_drm.h>

#include "tri_frame.h"

struct bo {
	uint32_t handle, va;
	uint64_t size;
	void *map;
};

static int bo_new(int fd, struct bo *b, uint64_t size, int map, uint32_t info_flags)
{
	struct drm_mbx_bo_create c = { .size = size };
	struct drm_mbx_bo_info i = { .flags = info_flags };

	if (ioctl(fd, DRM_IOCTL_MBX_BO_CREATE, &c))
		return -errno;
	i.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_MBX_BO_INFO, &i))
		return -errno;
	b->handle = c.handle;
	b->va = i.va;
	b->size = i.size;
	b->map = NULL;
	if (map) {
		b->map = mmap(NULL, i.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)i.offset);
		if (b->map == MAP_FAILED)
			return -errno;
	}
	return 0;
}

static int open_node(const char *path)
{
	char name[64];
	int fd, k;

	if (path)
		return open(path, O_RDWR);
	for (k = 128; k < 140; k++) {
		struct drm_mbx_get_param p = { .param = MBX_PARAM_CORE_ID };

		snprintf(name, sizeof(name), "/dev/dri/renderD%d", k);
		fd = open(name, O_RDWR);
		if (fd < 0)
			continue;
		if (!ioctl(fd, DRM_IOCTL_MBX_GET_PARAM, &p)) {
			fprintf(stderr, "mbxtri-drm: %s, core id %#llx\n", name, (unsigned long long)p.value);
			return fd;
		}
		close(fd);
	}
	return -1;
}

/* The first 16 KB of registers to prefix.when (-R): which ones a job changes */
static void reg_dump(int fd, const char *prefix, const char *when)
{
	static uint32_t regs[0x4000 / 4];
	struct drm_mbx_reg_dump d = { .ptr = (uintptr_t)regs, .offset = 0, .size = sizeof(regs) };
	char name[256];
	FILE *f;

	if (ioctl(fd, DRM_IOCTL_MBX_REG_DUMP, &d)) {
		fprintf(stderr, "mbxtri-drm: REG_DUMP: %s\n", strerror(errno));
		return;
	}
	snprintf(name, sizeof(name), "%s.%s", prefix, when);
	if ((f = fopen(name, "wb"))) {
		fwrite(regs, 1, sizeof(regs), f);
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	uint32_t tails_bytes = 65536, tex_b_bits = 0x0e000000;
	struct bo tex = { 0 };	/* -T: the tail buffer's size (the driver zeroes tails_zero bytes of it) */
	const char *out = "mbxtri-drm.ppm", *dev = NULL;
	struct bo surf, regions, tails;
	struct drm_mbx_submit s;
	uint32_t *stream, *px;
	float deg = 0;
	int o, fd, i, x, y, lit = 0, ret, dump = 0;
	const char *pbfile = NULL;
	int empty = 0, gradient = 0, xmode = 0;
	const char *regprefix = NULL, *rawfile = NULL;
	uint32_t fill = TRI_CLEAR;
	int dbl = 0;
	uint32_t r_ctl = 0x30000, r_samp = 0x00020007;
	FILE *f;

	while ((o = getopt(argc, argv, "a:d:o:DP:EGC:M:S:R:X:W:F:ZK:A:B:T:N:Y:H:")) != -1) {
		if (o == 'a') deg = (float)atof(optarg);
		else if (o == 'd') dev = optarg;
		else if (o == 'o') out = optarg;
		else if (o == 'D') dump = 1;
		else if (o == 'P') pbfile = optarg;
		else if (o == 'E') empty = 1;
		else if (o == 'G') gradient = 1;
		else if (o == 'W') rawfile = optarg;
		else if (o == 'N') { tri_tex = 1; tri_tex_mode = (uint32_t)strtoul(optarg, NULL, 0); }
		else if (o == 'Y') tri_tex_c = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'H') tex_b_bits = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'T') tails_bytes = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'A') tri_alpha_word = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'B') tri_clear_alpha = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'K') tri_clear_state = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'Z') dbl = 1;	/* -Z: a target twice the size, both halves filled: the background texture can come from the second half (driver bg_shift 16384) */
		else if (o == 'F') fill = (uint32_t)strtoul(optarg, NULL, 0);	/* -F: what the CPU fills the target with */
		else if (o == 'X') xmode = atoi(optarg);	/* 1: the clear only; 2: the triangle only */
		else if (o == 'R') regprefix = optarg;
		else if (o == 'C') r_ctl = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'M') r_samp = (uint32_t)strtoul(optarg, NULL, 0);
		else if (o == 'S') tri_state_word = (uint32_t)strtoul(optarg, NULL, 0);
		else return 2;
	}
	if ((fd = open_node(dev)) < 0) {
		perror("mbxtri-drm: no MBX render node");
		return 1;
	}
	/* the tails: 64 KB aligned and 64 KB (the TA rounds the address down); the driver zeroes them */
	if ((ret = bo_new(fd, &surf, TRI_W * TRI_H * 4 * (dbl ? 2 : 1), 1, 0)) || (ret = bo_new(fd, &regions, TRI_W * TRI_H > 4096 ? 65536 : 4096, dump, 0)) ||
	    (ret = bo_new(fd, &tails, tails_bytes, dump, MBX_BO_INFO_ALIGN_64K))) {
		fprintf(stderr, "mbxtri-drm: buffers: %s\n", strerror(-ret));
		return 1;
	}
	if (tri_tex) {
		/* an 8 x 8 ARGB8888 texture, linear, in four quadrants: red and green below, blue and yellow above */
		static const uint32_t quad[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00 };
		uint32_t *tp;

		if (bo_new(fd, &tex, 4096, 1, 0)) {
			fprintf(stderr, "mbxtri-drm: texture buffer\n");
			return 1;
		}
		tp = tex.map;
		for (i = 0; i < 64; i++)
			tp[i] = quad[(i / 8 >= 4) * 2 + (i % 8 >= 4)];
		/* clamp both ways, 8 x 8, format 6 (ARGB8888), stride 8 texels (bit 0 of A) */
		tri_tex_a = 1u << 31 | 1u << 29 | 6u << 14 | 1;
		tri_tex_b = tex_b_bits | tex.va >> 7;
	}
	px = surf.map;
	for (i = 0; i < TRI_W * TRI_H * (dbl ? 2 : 1); i++)
		px[i] = gradient ? 0xff000000u | (uint32_t)i : fill;	/* -G: every pixel its own value */
	stream = calloc(128, 4);

	memset(&s, 0, sizeof(s));
	tri_with_clear = xmode != 2;
	tri_with_triangle = xmode != 1;
	s.stream_words = tri_stream(stream, deg);
	if (empty) {			/* -E: no triangle, only the end of the scene */
		stream[0] = 1;			/* a chunk of type 0 (the TA port), one word */
		stream[1] = 0xf0000000;
		s.stream_words = 2;
	}
	s.stream = (uintptr_t)stream;
	s.ta_config = 0x80 | (uint32_t)(TRI_W / 16 - 1) << 20 | (uint32_t)(TRI_H / 16 - 1) << 10;
	s.ta_width = (uint32_t)TRI_W << 16;
	s.ta_height = (uint32_t)TRI_H << 16;
	s.ta_regions = regions.va;
	s.ta_tails = tails.va;
	s.r_regions = regions.va;
	s.r_ctl = r_ctl;			/* depth on chip (0x30000) */
	s.r_sampling = r_samp;
	s.r_format = 0xe;			/* ARGB8888 */
	s.r_width = (uint32_t)(TRI_W - 1) << 16;
	s.r_height = (uint32_t)(TRI_H - 1) << 16;
	s.r_addr = surf.va;
	s.r_stride = TRI_W;			/* words */
	s.r_6dc = 0x3f800001;			/* clear depth 1.0, as GLES writes it (0 on the chip is not 1.0) */
	if (regprefix)
		reg_dump(fd, regprefix, "before");
	if (ioctl(fd, DRM_IOCTL_MBX_SUBMIT, &s)) {
		fprintf(stderr, "mbxtri-drm: submit: %s\n", strerror(errno));
		return 1;
	}
	{
		/* SUBMIT only queues the job: wait for its fence before the CPU looks at what it drew */
		struct drm_mbx_wait_fence w = { .fence = s.fence, .timeout_ns = 10ull * 1000000000ull };

		if (ioctl(fd, DRM_IOCTL_MBX_WAIT_FENCE, &w)) {
			fprintf(stderr, "mbxtri-drm: wait for the job: %s\n", strerror(errno));
			if (!dump)
				return 1;
			/* -D: look at what the TA left even though the render did not finish */
		}
	}
	if (regprefix)
		reg_dump(fd, regprefix, "after");
	if (pbfile) {
		/* 64 bytes of the free list's table, then the first 8 pages of the parameter buffer */
		static uint8_t buf[64 + 8 * 4096];
		struct drm_mbx_pb_dump d = { .ptr = (uintptr_t)buf, .offset = 0, .size = 64 };

		if (!ioctl(fd, DRM_IOCTL_MBX_PB_DUMP, &d)) {
			d.ptr = (uintptr_t)(buf + 64);
			d.offset = 16384;	/* MBX_PB_TABLE_SIZE: the pages follow the table */
			d.size = 8 * 4096;
			if (!ioctl(fd, DRM_IOCTL_MBX_PB_DUMP, &d) && (f = fopen(pbfile, "wb"))) {
				fwrite(buf, 1, sizeof(buf), f);
				fclose(f);
				fprintf(stderr, "mbxtri-drm: wrote %s\n", pbfile);
			}
		} else {
			fprintf(stderr, "mbxtri-drm: PB_DUMP: %s\n", strerror(errno));
		}
	}
	if (dump) {
		struct { const char *name; struct bo *b; } bufs[] = { { "regions", &regions }, { "tails", &tails } };

		for (i = 0; i < 2; i++) {
			uint32_t *w = bufs[i].b->map;
			int n = 0, k;

			if (i == 0) {	/* the region array whole: entries without a list (bit 25), end marks (bit 31), bit 26 */
				uint32_t nent = TRI_W * TRI_H > 4096 ? 65536 / 8 : 4096 / 8, e0, no_list = 0, b26 = 0, b31 = 0, last = 0;

				for (e0 = 0; e0 < nent; e0++) {
					uint32_t v = w[2 * e0];

					if (!v && !w[2 * e0 + 1])
						continue;
					last = e0;
					no_list += !!(v & 0x02000000);
					b26 += !!(v & 0x04000000);
					if (v & 0x80000000)
						b31++, fprintf(stderr, "mbxtri-drm: region entry %u has bit 31: %08x %08x\n", e0, v, w[2 * e0 + 1]);
				}
				fprintf(stderr, "mbxtri-drm: region array: last used entry %u, %u without a list (bit 25), %u with bit 26, %u with bit 31\n",
					last, no_list, b26, b31);
				for (e0 = last > 3 ? last - 3 : 0; e0 <= last; e0++)
					fprintf(stderr, "mbxtri-drm: region [%u] = %08x %08x\n", e0, w[2 * e0], w[2 * e0 + 1]);
			}
			fprintf(stderr, "mbxtri-drm: %s (va %#x):", bufs[i].name, bufs[i].b->va);
			for (k = 0; k < 1024; k++)
				if (w[k] && n++ < 64)
					fprintf(stderr, " [%d]=%08x", k, w[k]);
			fprintf(stderr, "%s (%d nonzero words)\n", n > 64 ? " ..." : "", n);
		}
	}
	if (dump) {	/* -D: a map of the tiles, '.' nothing drawn, 'o' some pixels, '#' more than half the tile */
		int tx, ty;

		for (ty = 0; ty < (TRI_H + 15) / 16; ty++) {
			char line[256];

			for (tx = 0; tx < (TRI_W + 15) / 16 && tx < 255; tx++) {
				int n = 0;

				for (y = ty * 16; y < ty * 16 + 16 && y < TRI_H; y++)
					for (x = tx * 16; x < tx * 16 + 16 && x < TRI_W; x++)
						n += px[y * TRI_W + x] != TRI_CLEAR;
				line[tx] = !n ? '.' : n > 128 ? '#' : 'o';
			}
			line[tx] = 0;
			fprintf(stderr, "map %2d %s\n", ty, line);
		}
	}
	if (rawfile && (f = fopen(rawfile, "wb"))) {	/* -W: the target's 32-bit words as the CPU reads them, alpha included */
		fwrite(px, 4, TRI_W * TRI_H, f);
		fclose(f);
	}
	if (!(f = fopen(out, "wb")))
		return 1;
	fprintf(f, "P6\n%d %d\n255\n", TRI_W, TRI_H);
	for (y = 0; y < TRI_H; y++)
		for (x = 0; x < TRI_W; x++) {
			uint32_t v = px[y * TRI_W + x];

			fputc(v >> 16 & 255, f);
			fputc(v >> 8 & 255, f);
			fputc(v & 255, f);
			lit += v != TRI_CLEAR;
		}
	fclose(f);
	fprintf(stderr, "mbxtri-drm: %d of %d pixels drawn, wrote %s\n", lit, TRI_W * TRI_H, out);
	return lit ? 0 : 1;
}
