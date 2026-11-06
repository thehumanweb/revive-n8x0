// SPDX-License-Identifier: MIT
/*
 * mbxtri-drm: mbxtri through the kernel's render node. Buffers by
 * DRM_IOCTL_MBX_BO_CREATE/INFO, the same stream (tri_frame.h) in one
 * DRM_IOCTL_MBX_SUBMIT, the picture read back through the buffer's mmap.
 *
 *   mbxtri-drm [-a degrees] [-d /dev/dri/renderD129] [-o out.ppm]
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
	int o, fd, i, x, y, lit = 0, ret;
	FILE *f;

	while ((o = getopt(argc, argv, "a:d:o:")) != -1) {
		if (o == 'a') deg = (float)atof(optarg);
		else if (o == 'd') dev = optarg;
		else if (o == 'o') out = optarg;
		else return 2;
	}
	if ((fd = open_node(dev)) < 0) {
		perror("mbxtri-drm: no MBX render node");
		return 1;
	}
	if ((ret = bo_new(fd, &surf, TRI_W * TRI_H * 4, 1)) || (ret = bo_new(fd, &regions, 4096, 0)) ||
	    (ret = bo_new(fd, &tails, 4096, 0)) || (ret = bo_new(fd, &pb, 4096, 0))) {
		fprintf(stderr, "mbxtri-drm: buffers: %s\n", strerror(-ret));
		return 1;
	}
	px = surf.map;
	for (i = 0; i < TRI_W * TRI_H; i++)
		px[i] = TRI_CLEAR;
	stream = calloc(128, 4);

	memset(&s, 0, sizeof(s));
	s.stream_words = tri_stream(stream, deg);
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
	s.r_pb = pb.va;
	if (ioctl(fd, DRM_IOCTL_MBX_SUBMIT, &s)) {
		fprintf(stderr, "mbxtri-drm: submit: %s\n", strerror(errno));
		return 1;
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
