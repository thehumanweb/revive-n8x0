// SPDX-License-Identifier: GPL-2.0
/*
 * n810-loader's /init: the whole userspace of a tiny kernel that NOLO boots
 * from the kernel partition, so the real kernel needn't fit in it. Mounts
 * the rootfs (OneNAND, jffs2) read-only, loads /boot/zImage (a zImage with
 * the N810 DTB appended, as we flash) and kexecs it with ATAGs for the
 * memory, the command line and NOLO's OMAP tags (boot reason, versions:
 * DSME stops in its malfunction state without a boot reason), which the
 * loader's decompressor kept in /chosen/linux,atag-board (patch 0003); the
 * kernel's ATAG_DTB_COMPAT folds them into its DTB. The kernel for this
 * device: /boot/zImage-n800 on an N800 (NOLO's "product" tag RX-34),
 * /boot/zImage otherwise (the N810's); then /boot/zImage.old; after that
 * a restart in 10 s. One rootfs serves both.
 *
 * The command line: /boot/cmdline if the rootfs has one; otherwise the
 * loader's own if a bootloader gave it (0xFFFF -b), not the loader's
 * built-in one (marked n810loader); otherwise none, and the kernel takes
 * its built-in one.
 *
 * No libc: ARM EABI system calls, so the initramfs is this one small file.
 */
#include <stddef.h>
#include <stdint.h>

#define NR_exit		1
#define NR_read		3
#define NR_write	4
#define NR_open		5
#define NR_close	6
#define NR_mount	21
#define NR_reboot	88
#define NR_nanosleep	162
#define NR_kexec_load	347

#define O_RDONLY	0
#define MS_RDONLY	1
#define REBOOT_MAGIC1	0xfee1dead
#define REBOOT_MAGIC2	672274793
#define REBOOT_KEXEC	0x45584543
#define REBOOT_RESTART	0x01234567

#define ATAG_CORE	0x54410001
#define ATAG_MEM	0x54410002
#define ATAG_CMDLINE	0x54410009
#define ATAG_BOARD	0x414f4d50	/* patches/6.18/0003 */
#define OMAP_TAG_VERSION_STR	0x4f82	/* char component[12], version[12] */

#define ZIMAGE_OFFSET	0x8000	/* KEXEC_ARM_ZIMAGE_OFFSET */
#define ATAGS_OFFSET	0x1000	/* KEXEC_ARM_ATAGS_OFFSET: r2 when no DTB segment */
#define ZIMAGE_MAX	(8 << 20)

struct kexec_segment {
	const void *buf;
	size_t bufsz;
	uintptr_t mem;
	size_t memsz;
};

static long sys(long nr, long a, long b, long c, long d, long e)
{
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r7 __asm__("r7") = nr;

	__asm__ __volatile__("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7)
			     : "memory");
	return r0;
}

static size_t len(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

static void say(const char *s)
{
	sys(NR_write, 1, (long)"n810-loader: ", 13, 0, 0);
	sys(NR_write, 1, (long)s, len(s), 0, 0);
}

static void hex(uint32_t v)
{
	char b[11] = "0x";
	int k;

	for (k = 0; k < 8; k++)
		b[2 + k] = "0123456789abcdef"[v >> (28 - 4 * k) & 15];
	b[10] = 0;
	sys(NR_write, 1, (long)b, 10, 0, 0);
}

static void *copy(void *d, const void *s, size_t n)
{
	uint8_t *p = d;
	const uint8_t *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

/* a whole file into buf (at most max bytes), its size or -1 */
static long slurp(const char *path, void *buf, long max)
{
	long fd = sys(NR_open, (long)path, O_RDONLY, 0, 0, 0), n = 0, r;

	if (fd < 0)
		return -1;
	while (n < max && (r = sys(NR_read, fd, (long)buf + n, max - n, 0, 0)) > 0)
		n += r;
	sys(NR_close, fd, 0, 0, 0, 0);
	return n;
}

static int has_word(const char *s, const char *w)
{
	size_t n = len(w), k;

	for (; *s; s++) {
		for (k = 0; k < n && s[k] == w[k]; k++)
			;
		if (k == n && (s[n] == 0 || s[n] == ' ' || s[n] == '\n'))
			return 1;
	}
	return 0;
}

static int starts(const char *s, const char *p)
{
	while (*p && *s == *p)
		s++, p++;
	return !*p;
}

static uint8_t board[1024];	/* NOLO's OMAP tags, the ATAG_BOARD payload */
static long nboard;

/* NOLO's "product" version tag (RX-34 the N800, RX-44 the N810) into out
 * (13 bytes), or "" */
static void board_product(char *out)
{
	const uint8_t *p = board, *end = board + (nboard > 0 ? nboard : 0);

	out[0] = 0;
	while (p + 4 <= end) {
		uint16_t tag = p[0] | p[1] << 8, tlen = p[2] | p[3] << 8;
		const uint8_t *data = p + 4;

		if (data + tlen > end)
			break;
		if (tag == OMAP_TAG_VERSION_STR && tlen >= 24 && starts((const char *)data, "product")) {
			copy(out, data + 12, 12);
			out[12] = 0;
			return;
		}
		p = data + ((tlen + 3) & ~3u);
	}
}

static uint32_t parse_hex(const char *s, const char **end)
{
	uint32_t v = 0;

	for (;; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;

		if (d < 0)
			break;
		v = v << 4 | (uint32_t)d;
	}
	*end = s;
	return v;
}

/* the first System RAM range in /proc/iomem ("80000000-87ffffff : System RAM") */
static int ram(uint32_t *start, uint32_t *size)
{
	static char io[4096];
	long n = slurp("/proc/iomem", io, sizeof(io) - 1);
	const char *s = io, *e;

	if (n <= 0)
		return -1;
	io[n] = 0;
	while (*s) {
		uint32_t a = parse_hex(s, &e), b;

		if (*e == '-') {
			b = parse_hex(e + 1, &e);
			if (starts(e, " : System RAM")) {
				*start = a;
				*size = b - a + 1;
				return 0;
			}
		}
		while (*s && *s != '\n')
			s++;
		if (*s)
			s++;
	}
	return -1;
}

static uint8_t zimage[ZIMAGE_MAX] __attribute__((aligned(4096)));
static uint32_t atags[1024] __attribute__((aligned(4096)));

/* /boot/<name> kexec'd with cmdline (or none): returns only on failure */
static void boot(const char *path, const char *cmdline, uint32_t base, uint32_t size)
{
	struct kexec_segment seg[2];
	long n = slurp(path, zimage, ZIMAGE_MAX);
	uint32_t *t = atags, magic;

	if (n < 0x30) {
		say("no ");
		sys(NR_write, 1, (long)path, len(path), 0, 0);
		sys(NR_write, 1, (long)"\n", 1, 0, 0);
		return;
	}
	copy(&magic, zimage + 0x24, 4);
	if (magic != 0x016f2818) {
		say("not a zImage: ");
		sys(NR_write, 1, (long)path, len(path), 0, 0);
		sys(NR_write, 1, (long)"\n", 1, 0, 0);
		return;
	}
	*t++ = 5, *t++ = ATAG_CORE, *t++ = 1, *t++ = 4096, *t++ = 0;
	*t++ = 4, *t++ = ATAG_MEM, *t++ = size, *t++ = base;
	if (nboard > 0) {
		*t++ = 2 + (uint32_t)(nboard + 3) / 4;
		*t++ = ATAG_BOARD;
		copy(t, board, (size_t)nboard);
		t += (nboard + 3) / 4;
	}
	if (cmdline && *cmdline) {
		size_t l = len(cmdline);

		if (l > 1023 * 4 - 64 - 4 * (size_t)(t - atags))
			l = 1023 * 4 - 64 - 4 * (size_t)(t - atags);
		*t++ = 2 + (l + 1 + 3) / 4;
		*t++ = ATAG_CMDLINE;
		copy(t, cmdline, l);
		((char *)t)[l] = 0;
		t += (l + 1 + 3) / 4;
	}
	*t++ = 0, *t++ = 0;
	seg[0] = (struct kexec_segment){ atags, sizeof(atags), base + ATAGS_OFFSET, sizeof(atags) };
	seg[1] = (struct kexec_segment){ zimage, (size_t)n, base + ZIMAGE_OFFSET,
					 ((size_t)n + 4095) & ~(size_t)4095 };
	say("kexec ");
	sys(NR_write, 1, (long)path, len(path), 0, 0);
	sys(NR_write, 1, (long)", ", 2, 0, 0);
	hex((uint32_t)n);
	sys(NR_write, 1, (long)" bytes\n", 7, 0, 0);
	if (sys(NR_kexec_load, base + ZIMAGE_OFFSET, 2, (long)seg, 0, 0) < 0) {
		say("kexec_load failed\n");
		return;
	}
	sys(NR_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_KEXEC, 0, 0);
	say("reboot into it failed\n");
}

void _start(void) __attribute__((noreturn));
void _start(void)
{
	static char cmdline[4096], own[4096];
	const char *pass = NULL;
	uint32_t base = 0x80000000, size = 128 << 20;
	long n;

	sys(NR_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0);
	sys(NR_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
	nboard = slurp("/sys/firmware/devicetree/base/chosen/linux,atag-board", board, sizeof(board));
	if (nboard <= 0)
		say("no NOLO tags (/chosen/linux,atag-board): no boot reason for DSME\n");
	n = slurp("/proc/cmdline", own, sizeof(own) - 1);
	own[n > 0 ? n : 0] = 0;
	if (n > 0 && own[n - 1] == '\n')
		own[n - 1] = 0;
	if (ram(&base, &size))
		say("no System RAM in /proc/iomem: 128 MB at 0x80000000\n");
	if (sys(NR_mount, (long)"mtd:rootfs", (long)"/r", (long)"jffs2", MS_RDONLY, 0) < 0) {
		say("can't mount the rootfs (mtd:rootfs, jffs2)\n");
	} else {
		n = slurp("/r/boot/cmdline", cmdline, sizeof(cmdline) - 1);
		if (n > 0) {
			cmdline[n] = 0;
			while (n > 0 && (cmdline[n - 1] == '\n' || cmdline[n - 1] == ' '))
				cmdline[--n] = 0;
			pass = cmdline;
		} else if (*own && !has_word(own, "n810loader")) {
			pass = own;	/* from the bootloader: on to the kernel */
		}
		char product[13];

		board_product(product);
		if (starts(product, "RX-34")) {	/* the N800 */
			say("an N800\n");
			boot("/r/boot/zImage-n800", pass, base, size);
		}
		boot("/r/boot/zImage", pass, base, size);
		boot("/r/boot/zImage.old", pass, base, size);
	}
	say("nothing to boot: restarting in 10 s\n");
	{
		struct { long s, ns; } t = { 10, 0 };

		sys(NR_nanosleep, (long)&t, 0, 0, 0, 0);
	}
	sys(NR_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_RESTART, 0, 0);
	for (;;)
		sys(NR_exit, 1, 0, 0, 0, 0);
}
