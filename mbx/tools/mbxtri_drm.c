// SPDX-License-Identifier: MIT
/*
 * mbxtri-drm: mbxtri through the kernel's render node. Buffers by
 * DRM_IOCTL_MBX_BO_CREATE/INFO, the same stream (tri_frame.h) in one
 * DRM_IOCTL_MBX_SUBMIT, the picture read back through the buffer's mmap.
 *
 *   mbxtri-drm [-a degrees] [-d /dev/dri/renderD129] [-o out.ppm] [-D] [-P file] [-B addr] [-K] [-E]
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

static int bo_new(int fd, struct bo *b, uint64_t size, int map)
{
	struct drm_mbx_bo_create c = { .size = size };
	struct drm_mbx_bo_info i = { 0 };

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

int main(int argc, char **argv)
{
	const char *out = "mbxtri-drm.ppm", *dev = NULL;
	struct bo surf, regions, tails, pb;
	struct drm_mbx_submit s;
	uint32_t *stream, *px;
	float deg = 0;
	int o, fd, i, x, y, lit = 0, ret, dump = 0;
	const char *pbfile = NULL;
	uint32_t r_pb_override = 0, sflags = 0;
	int empty = 0;
	FILE *f;

	while ((o = getopt(argc, argv, "a:d:o:DP:B:KE")) != -1) {
		if (o == 'a') deg = (float)atof(optarg);
		else if (o == 'd') dev = optarg;
		else if (o == 'o') out = optarg;
		else if (o == 'D') dump = 1;
		else if (o == 'P') pbfile = optarg;
		else if (o == 'E') empty = 1;
		else if (o == 'K') sflags = MBX_SUBMIT_BG_TARGET;
		else if (o == 'B') r_pb_override = (uint32_t)strtoul(optarg, NULL, 0);
		else return 2;
	}
	if ((fd = open_node(dev)) < 0) {
		perror("mbxtri-drm: no MBX render node");
		return 1;
	}
	if ((ret = bo_new(fd, &surf, TRI_W * TRI_H * 4, 1)) || (ret = bo_new(fd, &regions, 4096, dump)) ||
	    (ret = bo_new(fd, &tails, 4096, dump)) || (ret = bo_new(fd, &pb, 4096, dump))) {
		fprintf(stderr, "mbxtri-drm: buffers: %s\n", strerror(-ret));
		return 1;
	}
	px = surf.map;
	for (i = 0; i < TRI_W * TRI_H; i++)
		px[i] = TRI_CLEAR;
	stream = calloc(128, 4);

	memset(&s, 0, sizeof(s));
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
	s.r_ctl = 0x30000;			/* depth on chip */
	s.r_sampling = 0x00020007;
	s.r_format = 0xe;			/* ARGB8888 */
	s.r_width = (uint32_t)(TRI_W - 1) << 16;
	s.r_height = (uint32_t)(TRI_H - 1) << 16;
	s.r_addr = surf.va;
	s.r_stride = TRI_W;			/* words */
	s.flags = sflags;
	s.r_pb = r_pb_override ? r_pb_override : pb.va;	/* -B: the parameter buffer's own base instead of a separate background object */
	if (ioctl(fd, DRM_IOCTL_MBX_SUBMIT, &s)) {
		fprintf(stderr, "mbxtri-drm: submit: %s\n", strerror(errno));
		return 1;
	}
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
		struct { const char *name; struct bo *b; } bufs[] = { { "regions", &regions }, { "tails", &tails }, { "pb", &pb } };

		for (i = 0; i < 3; i++) {
			uint32_t *w = bufs[i].b->map;
			int n = 0, k;

			fprintf(stderr, "mbxtri-drm: %s (va %#x):", bufs[i].name, bufs[i].b->va);
			for (k = 0; k < 1024; k++)
				if (w[k] && n++ < 64)
					fprintf(stderr, " [%d]=%08x", k, w[k]);
			fprintf(stderr, "%s (%d nonzero words)\n", n > 64 ? " ..." : "", n);
		}
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
