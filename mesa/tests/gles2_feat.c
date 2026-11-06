// SPDX-License-Identifier: MIT
/*
 * Feature tests for the mbx driver through GLES2: each draws something whose pixels follow from the
 * geometry (a 64x64 framebuffer, GL's bottom-left origin) and probes them. Prints "PASS name" or
 * "FAIL name: ..." per test; the exit status is the number of failures.
 *
 *   gles2_feat [name ...]     (no name: all)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static const char *vs_src =
	"attribute vec2 a_pos;\n"
	"attribute vec4 a_color;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"	gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"	v_color = a_color;\n"
	"	gl_PointSize = 8.0;\n"
	"}\n";
static const char *fs_src =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"void main() { gl_FragColor = v_color; }\n";

static GLuint prog, prog_tex, prog_mod, prog_uni, prog_vu, prog_tu, prog_z;
static int failures;

static const char *tvs_src =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"attribute vec4 a_color;\n"
	"varying vec2 v_uv;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"	gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"	v_uv = a_uv;\n"
	"	v_color = a_color;\n"
	"}\n";
static const char *tfs_replace =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
static const char *tfs_modulate =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"varying vec4 v_color;\n"
	"void main() { gl_FragColor = texture2D(u_tex, v_uv) * v_color; }\n";

static const char *fs_uniform =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"void main() { gl_FragColor = u_color; }\n";
static const char *fs_var_uniform =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"varying vec4 v_color;\n"
	"void main() { gl_FragColor = v_color * u_color; }\n";
static const char *fs_tex_uniform =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_color;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";

static const char *zvs_src =
	"attribute vec3 a_pos;\n"
	"attribute vec4 a_color;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"	gl_Position = vec4(a_pos, 1.0);\n"
	"	v_color = a_color;\n"
	"}\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(2);
	}
	return s;
}

struct px { unsigned char c[4]; };

static struct px probe(int x, int y)
{
	struct px p;

	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p.c);
	return p;
}

/* a colour is "near" when each channel is within 8 of the expectation */
static int is(int x, int y, int r, int g, int b, char *why, size_t n)
{
	struct px p = probe(x, y);

	if (abs(p.c[0] - r) > 8 || abs(p.c[1] - g) > 8 || abs(p.c[2] - b) > 8) {
		snprintf(why, n, "(%d,%d) is %d %d %d, expected %d %d %d", x, y, p.c[0], p.c[1], p.c[2], r, g, b);
		return 0;
	}
	return 1;
}

static void begin(void)
{
	glDisable(GL_SCISSOR_TEST);
	glColorMask(1, 1, 1, 1);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* vertices: x, y, r, g, b, a */
static void draw(GLenum mode, const float *v, int n)
{
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), v);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), v + 2);
	glDrawArrays(mode, 0, n);
}

#define CHECK(x, y, r, g, b) do { if (!is(x, y, r, g, b, why, 200)) return 0; } while (0)

static int t_point(char *why)
{
	/* a red point of size 8 (gl_PointSize in the shader) at (0.5, 0.5) = pixel (48, 48) */
	static const float v[] = { 0.5f, 0.5f, 1, 0, 0, 1 };

	begin();
	draw(GL_POINTS, v, 1);
	CHECK(48, 48, 255, 0, 0);
	CHECK(46, 46, 255, 0, 0);
	CHECK(50, 50, 255, 0, 0);
	CHECK(38, 48, 0, 0, 0);
	CHECK(56, 48, 0, 0, 0);
	CHECK(48, 38, 0, 0, 0);
	return 1;
}

static int t_line(char *why)
{
	/* a green horizontal line at y = -0.5 (pixel row 16), width 4, from x = -0.75 to 0.75 (pixels 8 to 56) */
	static const float v[] = { -0.75f, -0.5f, 0, 1, 0, 1, 0.75f, -0.5f, 0, 1, 0, 1 };

	begin();
	glLineWidth(4);
	draw(GL_LINES, v, 2);
	CHECK(32, 16, 0, 255, 0);
	CHECK(32, 17, 0, 255, 0);
	CHECK(32, 15, 0, 255, 0);
	CHECK(20, 16, 0, 255, 0);
	CHECK(32, 22, 0, 0, 0);
	CHECK(32, 10, 0, 0, 0);
	CHECK(4, 16, 0, 0, 0);
	CHECK(60, 16, 0, 0, 0);
	return 1;
}

static int t_line_strip(char *why)
{
	/* (-0.5,-0.5) -> (0.5,-0.5) -> (0.5,0.5): the bottom row 16 and the right column 48 */
	static const float v[] = { -0.5f, -0.5f, 0, 1, 0, 1, 0.5f, -0.5f, 0, 1, 0, 1, 0.5f, 0.5f, 0, 1, 0, 1 };

	begin();
	glLineWidth(2);
	draw(GL_LINE_STRIP, v, 3);
	CHECK(32, 16, 0, 255, 0);
	CHECK(48, 32, 0, 255, 0);
	CHECK(32, 32, 0, 0, 0);
	CHECK(20, 40, 0, 0, 0);
	return 1;
}

static int t_lines_restart(char *why)
{
	/* two separate vertical segments, x = -0.5 and 0.5; nothing joins them */
	static const float v[] = { -0.5f, -0.5f, 0, 1, 0, 1, -0.5f, 0.5f, 0, 1, 0, 1,
				   0.5f, -0.5f, 0, 1, 0, 1, 0.5f, 0.5f, 0, 1, 0, 1 };

	begin();
	glLineWidth(2);
	draw(GL_LINES, v, 4);
	CHECK(16, 32, 0, 255, 0);
	CHECK(48, 32, 0, 255, 0);
	CHECK(32, 32, 0, 0, 0);
	CHECK(32, 16, 0, 0, 0);
	CHECK(32, 48, 0, 0, 0);
	return 1;
}

static int t_line_loop(char *why)
{
	/* a square outline (a loop is converted by the state tracker) */
	static const float v[] = { -0.5f, -0.5f, 0, 1, 0, 1, 0.5f, -0.5f, 0, 1, 0, 1,
				   0.5f, 0.5f, 0, 1, 0, 1, -0.5f, 0.5f, 0, 1, 0, 1 };

	begin();
	glLineWidth(2);
	draw(GL_LINE_LOOP, v, 4);
	CHECK(32, 16, 0, 255, 0);
	CHECK(48, 32, 0, 255, 0);
	CHECK(32, 48, 0, 255, 0);
	CHECK(16, 32, 0, 255, 0);
	CHECK(32, 32, 0, 0, 0);
	return 1;
}

static GLuint link_tex(const char *fs)
{
	GLuint p = glCreateProgram();
	GLint ok = 0;

	glAttachShader(p, compile(GL_VERTEX_SHADER, tvs_src));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_uv");
	glBindAttribLocation(p, 2, "a_color");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024];

		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "link: %s\n", log);
	}
	return p;
}

/* A full-screen quad, uv from (u0, v0) at the bottom left to (u1, v1) at the top right */
static void quad(float u0, float v0, float u1, float v1, float r, float g, float b)
{
	const float v[] = {
		-1, -1, u0, v0, r, g, b, 1,
		1, -1, u1, v0, r, g, b, 1,
		1, 1, u1, v1, r, g, b, 1,
		-1, -1, u0, v0, r, g, b, 1,
		1, 1, u1, v1, r, g, b, 1,
		-1, 1, u0, v1, r, g, b, 1,
	};

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v + 2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v + 4);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(2);
}

static GLuint make_tex(int w, int h, GLenum fmt, GLenum type, const void *data, GLint filter, GLint wrap)
{
	GLuint t;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, type, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	return t;
}

/* a w x h RGBA texture in four quadrants: bottom left (row 0) red, bottom right green, top left blue, top right yellow */
static void quadrants(unsigned char *px, int w, int h)
{
	static const unsigned char col[4][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 } };

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			memcpy(px + 4 * (y * w + x), col[(y >= h / 2) * 2 + (x >= w / 2)], 4);
}

static int quad_probes(char *why)
{
	CHECK(16, 16, 255, 0, 0);
	CHECK(48, 16, 0, 255, 0);
	CHECK(16, 48, 0, 0, 255);
	CHECK(48, 48, 255, 255, 0);
	return 1;
}

static int t_tex_replace(char *why)
{
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok;

	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = quad_probes(why);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_modulate(char *why)
{
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok = 1;

	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_mod);
	glUniform1i(glGetUniformLocation(prog_mod, "u_tex"), 0);
	quad(0, 0, 1, 1, 0.5f, 0.5f, 0.5f);
	if (!is(16, 16, 128, 0, 0, why, 200) || !is(48, 16, 0, 128, 0, why, 200) || !is(16, 48, 0, 0, 128, why, 200) ||
	    !is(48, 48, 128, 128, 0, why, 200))
		ok = 0;
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_npot(char *why)
{
	/* 6 x 6: the cells are 3 texels, so the quadrants meet at the middle as in a power of two */
	unsigned char px[6 * 6 * 4];
	GLuint t;
	int ok;

	quadrants(px, 6, 6);
	begin();
	t = make_tex(6, 6, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = quad_probes(why) && is(2, 2, 255, 0, 0, why, 200) && is(61, 61, 255, 255, 0, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_bilinear(char *why)
{
	/* black left half, white right half; at the seam (x = 32) a bilinear sample is mixed, a nearest one is not */
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok = 1;
	struct px a, b;

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			unsigned char c = x >= 4 ? 255 : 0;

			px[4 * (y * 8 + x)] = px[4 * (y * 8 + x) + 1] = px[4 * (y * 8 + x) + 2] = c;
			px[4 * (y * 8 + x) + 3] = 255;
		}
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_LINEAR, GL_CLAMP_TO_EDGE);
	quad(0, 0, 1, 1, 1, 1, 1);
	a = probe(31, 32);
	b = probe(32, 32);
	if (!(a.c[0] > 20 && a.c[0] < 235) && !(b.c[0] > 20 && b.c[0] < 235)) {
		snprintf(why, 200, "bilinear: (31,32) is %d and (32,32) is %d: no mixed value at the seam", a.c[0], b.c[0]);
		ok = 0;
	}
	glDeleteTextures(1, &t);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	quad(0, 0, 1, 1, 1, 1, 1);
	a = probe(31, 32);
	b = probe(32, 32);
	if (ok && (!(a.c[0] < 8) || !(b.c[0] > 247))) {
		snprintf(why, 200, "nearest: (31,32) is %d and (32,32) is %d: not a sharp seam", a.c[0], b.c[0]);
		ok = 0;
	}
	glDeleteTextures(1, &t);
	glUseProgram(prog);
	return ok;
}

static int t_tex_repeat(char *why)
{
	/* only texel column 0 is red; u runs 0 to 4 across the screen: red every 16 pixels */
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok;

	memset(px, 0, sizeof(px));
	for (int y = 0; y < 8; y++) {
		px[4 * (y * 8)] = 255;
		for (int x = 0; x < 8; x++)
			px[4 * (y * 8 + x) + 3] = 255;
	}
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_REPEAT);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	quad(0, 0, 4, 1, 1, 1, 1);
	ok = is(1, 32, 255, 0, 0, why, 200) && is(17, 32, 255, 0, 0, why, 200) && is(33, 32, 255, 0, 0, why, 200) &&
	     is(9, 32, 0, 0, 0, why, 200) && is(25, 32, 0, 0, 0, why, 200);
	if (!ok && getenv("FEAT_ROW")) {
		for (int x = 0; x < 64; x++)
			fprintf(stderr, "%c", probe(x, 32).c[0] > 128 ? '#' : '.');
		fprintf(stderr, "\n");
	}
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_565(char *why)
{
	unsigned short px[8 * 8];
	GLuint t;
	int ok;

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			px[y * 8 + x] = (y >= 4 ? (x >= 4 ? 0xffe0 : 0x001f) : (x >= 4 ? 0x07e0 : 0xf800));	/* RGB565 */
	begin();
	t = make_tex(8, 8, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = quad_probes(why);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_4444(char *why)
{
	unsigned short px[8 * 8];
	GLuint t;
	int ok;

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			px[y * 8 + x] = (y >= 4 ? (x >= 4 ? 0xff0f : 0x00ff) : (x >= 4 ? 0x0f0f : 0xf00f));	/* RGBA4444 */
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = quad_probes(why);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_scissor_draw(char *why)
{
	/* a green full-screen quad inside the scissor rectangle (16, 16) to (48, 48) */
	static const float v[] = { -1, -1, 0, 1, 0, 1, 1, -1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1,
				   -1, -1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, -1, 1, 0, 1, 0, 1 };

	begin();
	glEnable(GL_SCISSOR_TEST);
	glScissor(16, 16, 32, 32);
	draw(GL_TRIANGLES, v, 6);
	glDisable(GL_SCISSOR_TEST);
	CHECK(32, 32, 0, 255, 0);
	CHECK(16, 16, 0, 255, 0);
	CHECK(47, 47, 0, 255, 0);
	CHECK(15, 32, 0, 0, 0);
	CHECK(48, 32, 0, 0, 0);
	CHECK(32, 15, 0, 0, 0);
	CHECK(32, 48, 0, 0, 0);
	CHECK(8, 8, 0, 0, 0);
	CHECK(56, 56, 0, 0, 0);
	return 1;
}

static int t_scissor_clear(char *why)
{
	begin();
	glEnable(GL_SCISSOR_TEST);
	glScissor(8, 24, 20, 16);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	CHECK(8, 24, 255, 0, 0);
	CHECK(27, 39, 255, 0, 0);
	CHECK(18, 32, 255, 0, 0);
	CHECK(7, 32, 0, 0, 0);
	CHECK(28, 32, 0, 0, 0);
	CHECK(18, 23, 0, 0, 0);
	CHECK(18, 40, 0, 0, 0);
	return 1;
}

static int t_fs_uniform(char *why)
{
	int ok;

	begin();
	glUseProgram(prog_uni);
	glUniform4f(glGetUniformLocation(prog_uni, "u_color"), 0.0f, 0.5f, 1.0f, 1.0f);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(32, 32, 0, 128, 255, why, 200) && is(5, 5, 0, 128, 255, why, 200);
	/* and again with another value: the constant is a program constant refreshed at the draw */
	if (ok) {
		glUniform4f(glGetUniformLocation(prog_uni, "u_color"), 1.0f, 0.25f, 0.0f, 1.0f);
		quad(0, 0, 1, 1, 1, 1, 1);
		ok = is(32, 32, 255, 64, 0, why, 200);
	}
	glUseProgram(prog);
	return ok;
}

static int t_fs_var_uniform(char *why)
{
	int ok;

	begin();
	glUseProgram(prog_vu);
	glUniform4f(glGetUniformLocation(prog_vu, "u_color"), 0.5f, 1.0f, 0.5f, 1.0f);
	quad(0, 0, 1, 1, 1.0f, 0.5f, 1.0f);	/* v_color (1, .5, 1) times (.5, 1, .5) */
	ok = is(32, 32, 128, 128, 128, why, 200);
	glUseProgram(prog);
	return ok;
}

static int t_tex_uniform(char *why)
{
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok;

	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_tu);
	glUniform1i(glGetUniformLocation(prog_tu, "u_tex"), 0);
	glUniform4f(glGetUniformLocation(prog_tu, "u_color"), 0.5f, 0.5f, 0.5f, 1.0f);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(16, 16, 128, 0, 0, why, 200) && is(48, 48, 128, 128, 0, why, 200) && is(16, 48, 0, 0, 128, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

/* a triangle (x, y, z, r, g, b, a) per vertex */
static void draw_z(GLenum mode, const float *v, int n)
{
	glUseProgram(prog_z);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), v);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), v + 3);
	glDrawArrays(mode, 0, n);
	glUseProgram(prog);
}

static void tri_z(float z, float r, float g, float b, float a, int cw)
{
	/* a large triangle covering the centre; cw swaps two vertices */
	float v[3][7] = {
		{ -0.9f, -0.9f, z, r, g, b, a }, { 0.9f, -0.9f, z, r, g, b, a }, { 0.0f, 0.9f, z, r, g, b, a },
	};
	float t[7];

	if (cw) {
		memcpy(t, v[1], sizeof(t));
		memcpy(v[1], v[2], sizeof(t));
		memcpy(v[2], t, sizeof(t));
	}
	draw_z(GL_TRIANGLES, &v[0][0], 3);
}

static int t_blend_alpha(char *why)
{
	/* red over a green background at alpha 0.5: (128, 128, 0) */
	begin();
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	tri_z(0, 1, 0, 0, 0.5f, 0);
	glDisable(GL_BLEND);
	CHECK(32, 24, 128, 128, 0);
	CHECK(2, 60, 0, 255, 0);
	return 1;
}

static int t_blend_add(char *why)
{
	/* dark red plus dark green: (64, 64, 0) */
	begin();
	glClearColor(0.25f, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	tri_z(0, 0, 0.25f, 0, 1, 0);
	glDisable(GL_BLEND);
	CHECK(32, 24, 64, 64, 0);
	CHECK(2, 60, 64, 0, 0);
	return 1;
}

static int t_cull(char *why)
{
	/* counter-clockwise is front; back faces are culled */
	begin();
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	tri_z(0, 1, 0, 0, 1, 1);	/* clockwise: culled */
	glDisable(GL_CULL_FACE);
	CHECK(32, 24, 0, 0, 0);
	glEnable(GL_CULL_FACE);
	tri_z(0, 0, 1, 0, 1, 0);	/* counter-clockwise: drawn */
	glDisable(GL_CULL_FACE);
	CHECK(32, 24, 0, 255, 0);
	begin();
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
	tri_z(0, 0, 1, 0, 1, 0);	/* front culled now */
	glDisable(GL_CULL_FACE);
	CHECK(32, 24, 0, 0, 0);
	return 1;
}

static int t_depth(char *why)
{
	/* the nearer triangle wins whichever is drawn first */
	begin();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	tri_z(0.2f, 1, 0, 0, 1, 0);	/* near, red */
	tri_z(0.6f, 0, 1, 0, 1, 0);	/* far, green: behind */
	CHECK(32, 24, 255, 0, 0);
	begin();
	tri_z(0.6f, 0, 1, 0, 1, 0);
	tri_z(0.2f, 1, 0, 0, 1, 0);
	CHECK(32, 24, 255, 0, 0);
	/* GL_GREATER: the far one wins */
	begin();
	glDepthFunc(GL_GREATER);
	glClearDepthf(0);
	glClear(GL_DEPTH_BUFFER_BIT);
	tri_z(0.2f, 1, 0, 0, 1, 0);
	tri_z(0.6f, 0, 1, 0, 1, 0);
	CHECK(32, 24, 0, 255, 0);
	glClearDepthf(1);
	glDepthFunc(GL_LESS);
	glDisable(GL_DEPTH_TEST);
	return 1;
}

static int t_depth_mask(char *why)
{
	/* a red triangle with the depth write off leaves the depth alone: a farther green one still shows over it */
	begin();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_FALSE);
	tri_z(0.2f, 1, 0, 0, 1, 0);
	glDepthMask(GL_TRUE);
	tri_z(0.6f, 0, 1, 0, 1, 0);
	CHECK(32, 24, 0, 255, 0);
	glDisable(GL_DEPTH_TEST);
	return 1;
}

static const struct { const char *name; int (*fn)(char *); } tests[] = {
	{ "blend_alpha", t_blend_alpha },
	{ "blend_add", t_blend_add },
	{ "cull", t_cull },
	{ "depth", t_depth },
	{ "depth_mask", t_depth_mask },
	{ "fs_uniform", t_fs_uniform },
	{ "fs_var_uniform", t_fs_var_uniform },
	{ "tex_uniform", t_tex_uniform },
	{ "scissor_draw", t_scissor_draw },
	{ "scissor_clear", t_scissor_clear },
	{ "tex_replace", t_tex_replace },
	{ "tex_modulate", t_tex_modulate },
	{ "tex_npot", t_tex_npot },
	{ "tex_bilinear", t_tex_bilinear },
	{ "tex_repeat", t_tex_repeat },
	{ "tex_565", t_tex_565 },
	{ "tex_4444", t_tex_4444 },
	{ "point", t_point },
	{ "line", t_line },
	{ "line_strip", t_line_strip },
	{ "lines_restart", t_lines_restart },
	{ "line_loop", t_line_loop },
};

int main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLConfig cfg;
	EGLint major, minor, ncfg;
	EGLContext ctx;
	GLuint fbo, rb, db;
	size_t i;
	int k;

	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor) ||
	    !eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || !ncfg) {
		fprintf(stderr, "gles2_feat: no EGL (0x%x)\n", eglGetError());
		return 2;
	}
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "gles2_feat: no context (0x%x)\n", eglGetError());
		return 2;
	}
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	glGenRenderbuffers(1, &db);
	glBindRenderbuffer(GL_RENDERBUFFER, db);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "gles2_feat: framebuffer incomplete\n");
		return 2;
	}
	prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog, 0, "a_pos");
	glBindAttribLocation(prog, 1, "a_color");
	glLinkProgram(prog);
	prog_tex = link_tex(tfs_replace);
	prog_mod = link_tex(tfs_modulate);
	prog_z = glCreateProgram();
	glAttachShader(prog_z, compile(GL_VERTEX_SHADER, zvs_src));
	glAttachShader(prog_z, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog_z, 0, "a_pos");
	glBindAttribLocation(prog_z, 1, "a_color");
	glLinkProgram(prog_z);
	prog_uni = link_tex(fs_uniform);
	prog_vu = link_tex(fs_var_uniform);
	prog_tu = link_tex(fs_tex_uniform);
	glUseProgram(prog);
	glViewport(0, 0, W, H);

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		char why[200] = "";

		if (argc > 1) {
			for (k = 1; k < argc; k++)
				if (!strcmp(argv[k], tests[i].name))
					break;
			if (k == argc)
				continue;
		}
		if (tests[i].fn(why)) {
			printf("PASS %s\n", tests[i].name);
		} else {
			printf("FAIL %s: %s\n", tests[i].name, why);
			failures++;
		}
	}
	return failures;
}
