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
#include <math.h>
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

static GLuint main_fbo;
static GLuint prog_sum, prog_mix, prog_txt, prog_two, prog, prog_tex, prog_mod, prog_uni, prog_vu, prog_tu, prog_z, prog_fba, prog_fbt, prog_fbz;
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

/* fragment shaders the fixed pipe cannot express: the CPU fallback draws them */
static const char *fs_arith =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"void main() { gl_FragColor = vec4(v_color.rgb * 0.5 + vec3(0.25), 1.0); }\n";
static const char *fs_tex_arith =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main() { vec4 t = texture2D(u_tex, v_uv); gl_FragColor = vec4(t.rgb * 0.5 + vec3(0.25), 1.0); }\n";

static const char *fs_two_tex =
	"precision mediump float;\n"
	"uniform sampler2D u_a;\n"
	"uniform sampler2D u_b;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = texture2D(u_a, v_uv) * texture2D(u_b, v_uv); }\n";

/* shader forms the combiner and the offset colour can do (the colour terms are computed per vertex) */
static const char *fs_tex_sum =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_add;\n"
	"varying vec2 v_uv;\n"
	"varying vec4 v_color;\n"
	"void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb * v_color.rgb + u_add.rgb, 1.0); }\n";
static const char *fs_tex_mix =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_color;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = vec4(mix(texture2D(u_tex, v_uv).rgb, u_color.rgb, 0.25), 1.0); }\n";
static const char *fs_text =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"varying vec4 v_color;\n"
	"void main() { gl_FragColor = vec4(v_color.rgb, v_color.a * texture2D(u_tex, v_uv).a); }\n";

static const char fs_cube[] =
	"precision mediump float;\n"
	"uniform samplerCube u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main() { gl_FragColor = textureCube(u_tex, vec3(v_uv.x - 0.5, (v_uv.y - 0.5) * 0.2, 0.0)); }\n";

static const char fs_discard[] =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"varying vec4 v_color;\n"
	"void main() { vec4 c = texture2D(u_tex, v_uv) * v_color; if (c.a < 0.5) discard; gl_FragColor = c; }\n";

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

/* gl_PointSize from a vertex attribute: a size per point */
static int t_point_sizes(char *why)
{
	static const char vs[] =
		"attribute vec2 a_pos;\nattribute vec4 a_color;\nattribute float a_size;\nvarying vec4 v_color;\n"
		"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_color = a_color; gl_PointSize = a_size; }\n";
	static const float v[] = { -0.5f, 0, 1, 0, 0, 1, 4,	0.5f, 0, 0, 1, 0, 1, 12 };
	GLuint p = glCreateProgram();
	int ok;

	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_color");
	glBindAttribLocation(p, 2, "a_size");
	glLinkProgram(p);
	glUseProgram(p);
	begin();
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), v);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), v + 2);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), v + 6);
	glDrawArrays(GL_POINTS, 0, 2);
	glDisableVertexAttribArray(2);
	ok = is(16, 32, 255, 0, 0, why, 200) && is(17, 31, 255, 0, 0, why, 200) && is(21, 32, 0, 0, 0, why, 200) &&
	     is(48, 32, 0, 255, 0, why, 200) && is(53, 32, 0, 255, 0, why, 200) && is(55, 32, 0, 0, 0, why, 200);
	glUseProgram(prog);
	glDeleteProgram(p);
	return ok;
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

/* a rectangle (x0, y0) to (x1, y1) in the textured programs' vertex layout, colour (r, g, b) */
static void rect(float x0, float y0, float x1, float y1, float r, float g, float b)
{
	const float v[] = {
		x0, y0, 0, 0, r, g, b, 1,
		x1, y0, 1, 0, r, g, b, 1,
		x1, y1, 1, 1, r, g, b, 1,
		x0, y0, 0, 0, r, g, b, 1,
		x1, y1, 1, 1, r, g, b, 1,
		x0, y1, 0, 1, r, g, b, 1,
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

/* a GL_RGB texture is stored with an unused byte the chip reads as alpha: it must stay opaque (blended over green nothing mixes) */
static int t_tex_rgb(char *why)
{
	unsigned char rgba[8 * 8 * 4], px[8 * 8 * 3];
	GLuint t;
	int ok;

	quadrants(rgba, 8, 8);
	for (int i = 0; i < 64; i++)
		memcpy(px + 3 * i, rgba + 4 * i, 3);
	begin();
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	t = make_tex(8, 8, GL_RGB, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	quad(0, 0, 1, 1, 1, 1, 1);
	glDisable(GL_BLEND);
	ok = quad_probes(why);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

/*
 * A dma-buf imported twice is one GEM handle: the importer's second use must survive the first one's release (Weston
 * re-imports a client's buffer at every attach). Export a texture's image, import it twice, release the first, and
 * sample the second.
 */
static int t_dmabuf_reimport(char *why)
{
	PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	PFNEGLDESTROYIMAGEKHRPROC destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
	PFNEGLEXPORTDMABUFIMAGEMESAPROC export_image = (PFNEGLEXPORTDMABUFIMAGEMESAPROC)eglGetProcAddress("eglExportDMABUFImageMESA");
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	EGLDisplay dpy = eglGetCurrentDisplay();
	EGLContext ctx = eglGetCurrentContext();
	unsigned char px[8 * 8 * 4];
	EGLImageKHR src, a, b;
	GLuint t, ta, tb;
	int fourcc, nplanes, fd = -1, stride = 0, offset = 0, ok = 1;
	EGLuint64KHR mod;

	if (!create_image || !destroy_image || !query || !export_image || !target) {
		snprintf(why, 200, "no dma-buf export or import");
		return 1;	/* nothing to test here: not a failure */
	}
	quadrants(px, 8, 8);
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	src = create_image(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)t, NULL);
	if (src == EGL_NO_IMAGE_KHR || !query(dpy, src, &fourcc, &nplanes, &mod) ||
	    !export_image(dpy, src, &fd, &stride, &offset)) {
		snprintf(why, 200, "the texture could not be exported");
		return 0;
	}
	{
		const EGLint attr[] = { EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_LINUX_DRM_FOURCC_EXT, fourcc, EGL_DMA_BUF_PLANE0_FD_EXT, fd,
					EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset, EGL_DMA_BUF_PLANE0_PITCH_EXT, stride, EGL_NONE };

		a = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
		b = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	}
	if (a == EGL_NO_IMAGE_KHR || b == EGL_NO_IMAGE_KHR) {
		/* the host sim's emulated DRM device cannot import a dma-buf; on the unit this is a failure */
		if (getenv("MBX_SIM_NO_DMABUF_IMPORT") || getenv("LD_PRELOAD")) {
			fprintf(stderr, "dmabuf_reimport: skipped, the dma-buf cannot be imported here (sim)\n");
			return 1;
		}
		snprintf(why, 200, "the dma-buf could not be imported");
		return 0;
	}
	glGenTextures(1, &ta);
	glBindTexture(GL_TEXTURE_2D, ta);
	target(GL_TEXTURE_2D, (GLeglImageOES)a);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenTextures(1, &tb);
	glBindTexture(GL_TEXTURE_2D, tb);
	target(GL_TEXTURE_2D, (GLeglImageOES)b);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	/* the first import, then its release, then the second */
	begin();
	glBindTexture(GL_TEXTURE_2D, ta);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = quad_probes(why);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &ta);
	destroy_image(dpy, a);
	glFinish();
	if (ok) {
		begin();
		glBindTexture(GL_TEXTURE_2D, tb);
		quad(0, 0, 1, 1, 1, 1, 1);
		ok = quad_probes(why);
		if (!ok)
			strncat(why, " (after the first import was released)", 60);
	}
	glUseProgram(prog);
	glDeleteTextures(1, &tb);
	destroy_image(dpy, b);
	destroy_image(dpy, src);
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
	/* dark grey left half, light grey right half (not black: the chip does not filter next to a texel whose
	 * colour is all zero); at the seam (x = 32) a bilinear sample is mixed, a nearest one is not */
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok = 1;
	struct px a, b;

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			unsigned char c = x >= 4 ? 224 : 32;

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
	if (getenv("FEAT_ROW")) {
		for (int x = 24; x < 40; x++)
			fprintf(stderr, "%d ", probe(x, 32).c[0]);
		fprintf(stderr, "(bilinear row, x = 24..39)\n");
		for (int y = 0; y < 64; y += 8)
			fprintf(stderr, "%d ", probe(48, y).c[0]);
		fprintf(stderr, "(column x = 48)\n");
	}
	if (!(a.c[0] > 50 && a.c[0] < 210) && !(b.c[0] > 50 && b.c[0] < 210)) {
		snprintf(why, 200, "bilinear: (31,32) is %d and (32,32) is %d: no mixed value at the seam", a.c[0], b.c[0]);
		ok = 0;
	}
	glDeleteTextures(1, &t);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	quad(0, 0, 1, 1, 1, 1, 1);
	a = probe(31, 32);
	b = probe(32, 32);
	if (ok && (!(a.c[0] < 40) || !(b.c[0] > 216))) {
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

static void tri_z(float z, float r, float g, float b, float a, int cw);

/* full-target clears the chip paints in its background pass (README assumption 44), and what must settle them first */
static int t_clear_flat(char *why)
{
	begin();
	glClearColor(0.25f, 0.5f, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	CHECK(0, 0, 64, 128, 255);			/* clear only, then read */
	CHECK(63, 47, 64, 128, 255);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);			/* a second clear replaces the first */
	tri_z(0.5f, 0, 0, 1, 1, 0);			/* clear + draw in one job */
	CHECK(2, 2, 0, 255, 0);
	CHECK(32, 24, 0, 0, 255);
	glEnable(GL_SCISSOR_TEST);
	glScissor(4, 4, 8, 8);
	glClearColor(1, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);			/* a scissored clear after a full one */
	glDisable(GL_SCISSOR_TEST);
	CHECK(6, 6, 255, 255, 0);
	CHECK(2, 2, 0, 255, 0);
	CHECK(32, 24, 0, 0, 255);
	return 1;
}

/* a cube map: sampled by the CPU fallback; the left half of the quad reads -X, the right half +X */
static int t_cube(char *why)
{
	static const unsigned char col[6][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 } };
	unsigned char px[4 * 4 * 4];
	GLuint t, p;
	int ok;

	begin();
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_CUBE_MAP, t);
	for (int f = 0; f < 6; f++) {
		for (int i = 0; i < 16; i++) {
			px[i * 4] = col[f][0]; px[i * 4 + 1] = col[f][1]; px[i * 4 + 2] = col[f][2]; px[i * 4 + 3] = 255;
		}
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	p = link_tex(fs_cube);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(8, 32, 0, 255, 0, why, 200) && is(56, 32, 255, 0, 0, why, 200);	/* -X green, +X red */
	/* change one face and draw again: the CPU's copy follows */
	if (ok) {
		for (int i = 0; i < 16; i++) {
			px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255;
		}
		glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
		quad(0, 0, 1, 1, 1, 1, 1);
		ok = is(56, 32, 0, 0, 255, why, 200) && is(8, 32, 0, 255, 0, why, 200);
	}
	glUseProgram(prog);
	glDeleteProgram(p);
	glDeleteTextures(1, &t);
	return ok;
}

/* discard on alpha: a cut-out (the chip's alpha function, state bit 14); the left half of the texture has alpha 0.25, the right 0.88 */
static int t_alpha_discard(char *why)
{
	unsigned char px[8 * 8 * 4];
	GLuint t, p;
	int ok;

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			unsigned char *q = px + (y * 8 + x) * 4;

			q[0] = 255; q[1] = 200; q[2] = 100; q[3] = x < 4 ? 64 : 224;
		}
	begin();
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	p = link_tex(fs_discard);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(8, 32, 0, 0, 255, why, 200) && is(56, 32, 255, 200, 100, why, 200) && is(20, 8, 0, 0, 255, why, 200);
	glUseProgram(prog);
	glDeleteProgram(p);
	glDeleteTextures(1, &t);
	return ok;
}

/* gl_FragDepthEXT written by the shader: the chip has no per-pixel depth output, so softpipe draws it (piglit's fragdepth_gles2) */
static int t_frag_depth(char *why)
{
	static const char vs[] =
		"attribute vec4 a_pos;\nvarying float z;\n"
		"void main() { gl_Position = a_pos; z = a_pos.x * 0.5 + 0.5; }\n";
	static const char fs[] =
		"#extension GL_EXT_frag_depth : enable\nprecision mediump float;\nvarying float z;\nuniform vec4 color;\n"
		"void main() { if (color.g == 1.0) gl_FragDepthEXT = z; else gl_FragDepthEXT = 0.5; gl_FragColor = color; }\n";
	static const float red[] = { -0.5f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1, -0.5f, 0.5f, 0, 1, 0.5f, 0.5f, 0, 1 };
	static const float green[] = { -0.75f, -0.25f, 0, 1, 0.75f, -0.25f, 0, 1, -0.75f, 0.25f, 0, 1, 0.75f, 0.25f, 0, 1 };
	GLuint p = glCreateProgram();
	GLint loc;
	int ok;

	if (!strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_frag_depth"))
		return 1;		/* not offered: nothing to test */
	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "a_pos");
	glLinkProgram(p);
	glUseProgram(p);
	loc = glGetUniformLocation(p, "color");
	begin();
	glEnable(GL_DEPTH_TEST);
	glEnableVertexAttribArray(0);
	glUniform4f(loc, 1, 0, 0, 1);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, red);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUniform4f(loc, 0, 1, 0, 1);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, green);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_DEPTH_TEST);
	/* the green rectangle's depth grows from left to right and the red one's is 0.5: green wins on the left of the middle */
	ok = is(26, 32, 0, 255, 0, why, 200) && is(38, 32, 255, 0, 0, why, 200);
	glUseProgram(prog);
	glDeleteProgram(p);
	return ok;
}

/* a shader that is not affine in its varying (normalize and dot per pixel): the chip evaluates it at the corners of a fine grid
 * and interpolates (README assumption 48); the quad's normals run from (-1,-1,1) to (1,1,1), so d = 1 / sqrt(u * u + v * v + 1) */
static int t_grid_light(char *why)
{
	static const char fs[] =
		"precision mediump float;\nvarying vec4 v_color;\n"
		"void main() { vec3 n = normalize(v_color.xyz); float d = max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0); gl_FragColor = vec4(d, d * 0.5, 1.0 - d, 1.0); }\n";
	static const float v[] = { -1, -1, -1, -1, 1, 1,	1, -1, 1, -1, 1, 1,	-1, 1, -1, 1, 1, 1,	1, 1, 1, 1, 1, 1 };
	GLuint p = glCreateProgram();
	struct { int x, y; } pt[] = { { 32, 32 }, { 8, 32 }, { 32, 8 }, { 6, 6 }, { 56, 40 }, { 20, 52 } };
	int ok = 1;

	glAttachShader(p, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_color");
	glLinkProgram(p);
	glUseProgram(p);
	begin();
	draw(GL_TRIANGLE_STRIP, v, 4);
	for (unsigned i = 0; i < sizeof(pt) / sizeof(pt[0]) && ok; i++) {
		float u = (pt[i].x + 0.5f) / 32.0f - 1.0f, w = 1.0f - (pt[i].y + 0.5f) / 32.0f;
		float d = 1.0f / sqrtf(u * u + w * w + 1.0f);
		struct px a = probe(pt[i].x, pt[i].y);

		if (abs(a.c[0] - (int)(d * 255.0f + 0.5f)) > 14 || abs(a.c[1] - (int)(d * 127.5f + 0.5f)) > 10 ||
		    abs(a.c[2] - (int)((1.0f - d) * 255.0f + 0.5f)) > 14) {
			snprintf(why, 200, "(%d,%d) is %d %d %d, expected about %d %d %d", pt[i].x, pt[i].y, a.c[0], a.c[1], a.c[2],
				 (int)(d * 255.0f + 0.5f), (int)(d * 127.5f + 0.5f), (int)((1.0f - d) * 255.0f + 0.5f));
			ok = 0;
		}
	}
	glUseProgram(prog);
	glDeleteProgram(p);
	return ok;
}

/* a lit texture: the texel times a per-pixel lighting term that is not affine (grid mode for the lighting, the texture unit for the
 * texel, the combiner multiplies them); normals as in grid_light, a flat grey texel (200, 150, 100) */
static int t_grid_tex(char *why)
{
	static const char fs[] =
		"precision mediump float;\nuniform sampler2D u_tex;\nvarying vec2 v_uv;\nvarying vec4 v_color;\n"
		"void main() { vec3 n = normalize(v_color.xyz); float d = max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0); "
		"gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb * d, 1.0); }\n";
	unsigned char px[8 * 8 * 4];
	GLuint t, p = link_tex(fs);
	struct { int x, y; } pt[] = { { 32, 32 }, { 8, 32 }, { 6, 6 }, { 56, 40 } };
	int ok = 1;
	float v[4 * 8] = { -1, -1, 0, 0, -1, -1, 1, 1,	1, -1, 1, 0, 1, -1, 1, 1,	-1, 1, 0, 1, -1, 1, 1, 1,	1, 1, 1, 1, 1, 1, 1, 1 };

	for (int i = 0; i < 64; i++) {
		px[i * 4] = 200; px[i * 4 + 1] = 150; px[i * 4 + 2] = 100; px[i * 4 + 3] = 255;
	}
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v + 2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v + 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	for (unsigned i = 0; i < sizeof(pt) / sizeof(pt[0]) && ok; i++) {
		float u = (pt[i].x + 0.5f) / 32.0f - 1.0f, w = 1.0f - (pt[i].y + 0.5f) / 32.0f;
		float d = 1.0f / sqrtf(u * u + w * w + 1.0f);
		struct px a = probe(pt[i].x, pt[i].y);

		if (abs(a.c[0] - (int)(200 * d + 0.5f)) > 16 || abs(a.c[1] - (int)(150 * d + 0.5f)) > 14 || abs(a.c[2] - (int)(100 * d + 0.5f)) > 12) {
			snprintf(why, 200, "(%d,%d) is %d %d %d, expected about %d %d %d", pt[i].x, pt[i].y, a.c[0], a.c[1], a.c[2],
				 (int)(200 * d + 0.5f), (int)(150 * d + 0.5f), (int)(100 * d + 0.5f));
			ok = 0;
		}
	}
	glUseProgram(prog);
	glDeleteProgram(p);
	glDeleteTextures(1, &t);
	return ok;
}

/* depth carried between chip and CPU draws (README assumption 7): green at 0.6 on the chip, white at 0.3 through the CPU fallback
 * (a red-only colour mask), blue at 0.45 on the chip again: the blue one must lose to the CPU's depth */
static int t_depth_chip_cpu_chip(char *why)
{
	begin();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	tri_z(0.6f, 0, 1, 0, 1, 0);
	CHECK(32, 24, 0, 255, 0);
	glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_TRUE);
	tri_z(0.3f, 1, 1, 1, 1, 0);
	glColorMask(1, 1, 1, 1);
	tri_z(0.45f, 0, 0, 1, 1, 0);
	glDisable(GL_DEPTH_TEST);
	CHECK(32, 24, 255, 255, 0);
	return 1;
}

/* a chip draw's depth reaches a later CPU draw: the one behind it is not drawn */
static int t_depth_chip_cpu(char *why)
{
	begin();
	glEnable(GL_DEPTH_TEST);
	tri_z(0.3f, 0, 1, 0, 1, 0);
	glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_TRUE);
	tri_z(0.45f, 1, 1, 1, 1, 0);
	glColorMask(1, 1, 1, 1);
	glDisable(GL_DEPTH_TEST);
	CHECK(32, 24, 0, 255, 0);
	return 1;
}

/* two CPU draws: the second, behind the first, is not drawn */
static int t_depth_cpu_cpu(char *why)
{
	begin();
	glEnable(GL_DEPTH_TEST);
	glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_TRUE);
	tri_z(0.3f, 1, 1, 1, 1, 0);
	glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
	tri_z(0.45f, 1, 1, 1, 1, 0);
	glColorMask(1, 1, 1, 1);
	glDisable(GL_DEPTH_TEST);
	CHECK(32, 24, 255, 0, 0);
	return 1;
}

/* stencil (README assumption 49): a framebuffer with a packed depth-stencil renderbuffer; a clip shape written with REPLACE (colour
 * off), content drawn where the stencil is 1, then INCR twice and a test for 2 */
static int stencil_run(char *why, int packed)
{
	GLuint fbo, rb, ds;
	GLint bits = 0;
	static const float shape[] = { -1, -1, 1, 1, 1, 1,	0.6f, -1, 1, 1, 1, 1,	-1, 0.6f, 1, 1, 1, 1 };
	static const float full[] = { -1, -1, 0, 1, 0, 1,	1, -1, 0, 1, 0, 1,	-1, 1, 0, 1, 0, 1,	1, -1, 0, 1, 0, 1,	1, 1, 0, 1, 0, 1,	-1, 1, 0, 1, 0, 1 };
	int ok = 1;

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	glGenRenderbuffers(1, &ds);
	glBindRenderbuffer(GL_RENDERBUFFER, ds);
	glRenderbufferStorage(GL_RENDERBUFFER, packed ? 0x88F0 /* GL_DEPTH24_STENCIL8_OES */ : GL_STENCIL_INDEX8, W, H);
	if (packed)
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ds);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		snprintf(why, 200, "the depth-stencil framebuffer is incomplete (%x)", glCheckFramebufferStatus(GL_FRAMEBUFFER));
		ok = 0;
		goto out;
	}
	glGetIntegerv(GL_STENCIL_BITS, &bits);
	if (bits < 8) {
		snprintf(why, 200, "stencil bits %d", bits);
		ok = 0;
		goto out;
	}
	glViewport(0, 0, W, H);
	begin();
	glClearColor(0, 0, 0, 1);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glColorMask(0, 0, 0, 0);
	draw(GL_TRIANGLES, shape, 3);
	glColorMask(1, 1, 1, 1);
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	draw(GL_TRIANGLES, full, 6);
	if (!is(8, 8, 0, 255, 0, why, 200) || !is(56, 56, 0, 0, 0, why, 200) || !is(56, 8, 0, 0, 0, why, 200)) {
		ok = 0;
		goto out;
	}
	/* counting: the shape drawn twice with INCR is 2 inside, so a test for 2 draws red there (and 1 elsewhere in the shape stays) */
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glStencilFunc(GL_ALWAYS, 0, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	glColorMask(0, 0, 0, 0);
	draw(GL_TRIANGLES, shape, 3);
	draw(GL_TRIANGLES, shape, 3);
	glColorMask(1, 1, 1, 1);
	glStencilFunc(GL_EQUAL, 2, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	draw(GL_TRIANGLES, full, 6);
	if (!is(8, 8, 0, 255, 0, why, 200) || !is(56, 56, 0, 0, 0, why, 200))
		ok = 0;
	glStencilFunc(GL_EQUAL, 1, 0xff);	/* nothing is 1 now: nothing is drawn */
	glClear(GL_COLOR_BUFFER_BIT);
	draw(GL_TRIANGLES, full, 6);
	if (ok && !is(8, 8, 0, 0, 0, why, 200))
		ok = 0;
out:
	glDisable(GL_STENCIL_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &rb);
	glDeleteRenderbuffers(1, &ds);
	return ok;
}

static int t_stencil(char *why)
{
	return stencil_run(why, 1);
}

static int t_stencil_only(char *why)
{
	return stencil_run(why, 0);
}

/* partial render (README assumption 50): with a damage rectangle only its tiles are rendered, the rest of the buffer keeps what it held
 * (the driver's hook MBX_TEST_DAMAGE stands in for set_damage_region, which needs a window surface) */
static void solid(float r, float g, float b)
{
	const float v[] = { -1, -1, r, g, b, 1,	1, -1, r, g, b, 1,	-1, 1, r, g, b, 1,	1, -1, r, g, b, 1,	1, 1, r, g, b, 1,	-1, 1, r, g, b, 1 };

	draw(GL_TRIANGLES, v, 6);
}

static int t_damage(char *why)
{
	begin();
	solid(1, 0, 0);				/* all red */
	CHECK(32, 32, 255, 0, 0);
	CHECK(4, 4, 255, 0, 0);
	setenv("MBX_TEST_DAMAGE", "16,16,32,32", 1);
	solid(0, 0, 1);				/* blue over all of it, but only the middle 2 x 2 tiles are rendered */
	CHECK(32, 32, 0, 0, 255);
	CHECK(20, 20, 0, 0, 255);
	CHECK(4, 4, 255, 0, 0);
	CHECK(4, 32, 255, 0, 0);
	CHECK(32, 4, 255, 0, 0);
	unsetenv("MBX_TEST_DAMAGE");
	solid(0, 0, 1);				/* without it the whole thing */
	CHECK(4, 4, 0, 0, 255);
	CHECK(32, 4, 0, 0, 255);
	return 1;
}

/* depth carried between chip and CPU draws in a framebuffer with stencil (README assumption 49): the CPU draws with the stencil test on */
static int t_stencil_depth(char *why)
{
	GLuint fbo, rb, ds;
	int ok = 1;

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	glGenRenderbuffers(1, &ds);
	glBindRenderbuffer(GL_RENDERBUFFER, ds);
	glRenderbufferStorage(GL_RENDERBUFFER, 0x88F0 /* GL_DEPTH24_STENCIL8_OES */, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ds);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
	glViewport(0, 0, W, H);
	begin();
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	/* no read-back in between (that would end the chip's job without keeping its depth): the colour channels tell the steps apart */
	tri_z(0.6f, 0, 1, 0, 1, 0);			/* the chip: green at 0.6 */
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glColorMask(1, 0, 0, 1);
	tri_z(0.8f, 1, 1, 1, 1, 0);			/* the CPU: red channel behind the chip's depth: must not be drawn */
	glColorMask(0, 0, 1, 1);
	tri_z(0.3f, 1, 1, 1, 1, 0);			/* the CPU: blue channel in front: drawn */
	glColorMask(1, 1, 1, 1);
	glDisable(GL_STENCIL_TEST);
	tri_z(0.5f, 1, 0, 0, 1, 0);			/* the chip again: red behind the CPU's depth: must not be drawn */
	if (!is(32, 24, 0, 255, 255, why, 200))
		ok = 0;
out:
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &rb);
	glDeleteRenderbuffers(1, &ds);
	return ok;
}

/* the stencil test on the chip (README assumption 53) with a depth test: a clip (the left half, written on the CPU), two draws tested against
 * it and against depth (the second behind the first), then a draw with no stencil: the rejected pixels' depth must be back as it was */
static int t_stencil_chip(char *why)
{
	static const float left[] = { -1, -1, 1, 1, 1, 1,	0, -1, 1, 1, 1, 1,	-1, 1, 1, 1, 1, 1,	0, -1, 1, 1, 1, 1,	0, 1, 1, 1, 1, 1,	-1, 1, 1, 1, 1, 1 };
	GLuint fbo, rb, ds;
	int ok = 1;

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	glGenRenderbuffers(1, &ds);
	glBindRenderbuffer(GL_RENDERBUFFER, ds);
	glRenderbufferStorage(GL_RENDERBUFFER, 0x88F0 /* GL_DEPTH24_STENCIL8_OES */, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ds);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
	glViewport(0, 0, W, H);
	begin();
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glColorMask(0, 0, 0, 0);
	draw(GL_TRIANGLES, left, 6);			/* the clip, on the CPU */
	glColorMask(1, 1, 1, 1);
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	tri_z(0.6f, 0, 1, 0, 1, 0);			/* tested draws, on the chip: green inside the clip only */
	tri_z(0.8f, 0, 0, 1, 1, 0);			/* blue behind the green, and outside the clip: nowhere */
	glDisable(GL_STENCIL_TEST);
	tri_z(0.7f, 1, 0, 0, 1, 0);			/* red: behind the green, but in front of the cleared depth the clip rejected */
	if (!is(16, 16, 0, 255, 0, why, 200) || !is(48, 16, 255, 0, 0, why, 200))
		ok = 0;
	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glDeleteFramebuffers(1, &fbo);
	glDeleteRenderbuffers(1, &rb);
	glDeleteRenderbuffers(1, &ds);
	return ok;
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

/* Weston's texture shader on GLES2: the channel swizzle comes in as integer uniforms. The identity value must reduce to a
 * plain texture on the chip; a real swizzle (here BGR) draws correctly on the CPU. */
static const char *tfs_weston =
	"precision mediump float;\n"
	"uniform sampler2D tex;\n"
	"uniform float view_alpha;\n"
	"uniform mediump ivec4 swizzle_idx[1];\n"
	"uniform vec4 swizzle_mask[1];\n"
	"uniform vec4 swizzle_sub[1];\n"
	"varying vec2 v_uv;\n"
	"float pick(int i, vec4 t) { return i < 2 ? (i < 1 ? t.x : t.y) : (i < 3 ? t.z : t.w); }\n"
	"void main() {\n"
	"	vec4 t = texture2D(tex, v_uv);\n"
	"	vec4 c = vec4(pick(swizzle_idx[0].x, t), pick(swizzle_idx[0].y, t), pick(swizzle_idx[0].z, t), pick(swizzle_idx[0].w, t));\n"
	"	gl_FragColor = (c * swizzle_mask[0] + swizzle_sub[0]) * view_alpha;\n"
	"}\n";

static int t_tex_weston(char *why)
{
	unsigned char px[8 * 8 * 4];
	static GLuint pw;
	GLuint t;
	int ok;

	if (!pw)
		pw = link_tex(tfs_weston);
	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(pw);
	glUniform1i(glGetUniformLocation(pw, "tex"), 0);
	glUniform1f(glGetUniformLocation(pw, "view_alpha"), 1.0f);
	glUniform4f(glGetUniformLocation(pw, "swizzle_mask[0]"), 1, 1, 1, 1);
	glUniform4f(glGetUniformLocation(pw, "swizzle_sub[0]"), 0, 0, 0, 0);
	glUniform4i(glGetUniformLocation(pw, "swizzle_idx[0]"), 0, 1, 2, 3);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(16, 16, 255, 0, 0, why, 200) && is(48, 16, 0, 255, 0, why, 200) && is(16, 48, 0, 0, 255, why, 200);
	if (ok) {		/* half the alpha: not a folded value, still the same program */
		begin();
		glUniform1f(glGetUniformLocation(pw, "view_alpha"), 0.5f);
		quad(0, 0, 1, 1, 1, 1, 1);
		ok = is(16, 16, 128, 0, 0, why, 200) && is(48, 48, 128, 128, 0, why, 200);
	}
	if (ok) {		/* blue and red swapped */
		begin();
		glUniform1f(glGetUniformLocation(pw, "view_alpha"), 1.0f);
		glUniform4i(glGetUniformLocation(pw, "swizzle_idx[0]"), 2, 1, 0, 3);
		quad(0, 0, 1, 1, 1, 1, 1);
		ok = is(16, 16, 0, 0, 255, why, 200) && is(16, 48, 255, 0, 0, why, 200) && is(48, 48, 0, 255, 255, why, 200);
	}
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

/* Weston's vertex shader (the texcoord-from-attribute variant): two mat4 uniforms and the position through one */
static const char *wvs_src =
	"precision highp float;\n"
	"uniform mat4 proj;\n"
	"uniform mat4 surface_to_buffer;\n"
	"attribute vec2 position;\n"
	"attribute vec2 texcoord;\n"
	"attribute vec4 barycentric;\n"
	"varying mediump vec2 v_uv;\n"
	
	"void main() {\n"
	"	gl_Position = proj * vec4(position, 0.0, 1.0);\n"
	"	v_uv = texcoord;\n"
	"}\n";

static int t_weston_vs(char *why)
{
	unsigned char px[8 * 8 * 4];
	static GLuint pw;
	GLuint t;
	int ok;
	const float proj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const float v[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, 1, 0, 1 };

	if (!pw) {
		GLint linked = 0;

		pw = glCreateProgram();
		glAttachShader(pw, compile(GL_VERTEX_SHADER, wvs_src));
		glAttachShader(pw, compile(GL_FRAGMENT_SHADER, tfs_weston));
		glBindAttribLocation(pw, 0, "position");
		glBindAttribLocation(pw, 1, "texcoord");
		glLinkProgram(pw);
		glGetProgramiv(pw, GL_LINK_STATUS, &linked);
		if (!linked)
			fprintf(stderr, "weston_vs: link failed\n");
	}
	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(pw);
	glUniform1i(glGetUniformLocation(pw, "tex"), 0);
	glUniform1f(glGetUniformLocation(pw, "view_alpha"), 1.0f);
	glUniform4f(glGetUniformLocation(pw, "swizzle_mask[0]"), 1, 1, 1, 1);
	glUniform4f(glGetUniformLocation(pw, "swizzle_sub[0]"), 0, 0, 0, 0);
	glUniform4i(glGetUniformLocation(pw, "swizzle_idx[0]"), 0, 1, 2, 3);
	glUniformMatrix4fv(glGetUniformLocation(pw, "proj"), 1, GL_FALSE, proj);
	glUniformMatrix4fv(glGetUniformLocation(pw, "surface_to_buffer"), 1, GL_FALSE, proj);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	ok = is(16, 16, 255, 0, 0, why, 200) && is(48, 16, 0, 255, 0, why, 200) && is(16, 48, 0, 0, 255, why, 200);
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

static int t_fb_arith(char *why)
{
	int ok;

	begin();
	glUseProgram(prog_fba);
	quad(0, 0, 1, 1, 1.0f, 0.0f, 0.5f);		/* (1, 0, .5) * .5 + .25 = (.75, .25, .5) */
	ok = is(32, 32, 191, 64, 128, why, 200) && is(4, 4, 191, 64, 128, why, 200) && is(60, 60, 191, 64, 128, why, 200);
	glUseProgram(prog);
	return ok;
}

static int t_fb_tex(char *why)
{
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok;

	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_fbt);
	glUniform1i(glGetUniformLocation(prog_fbt, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	/* red (1,0,0) -> (.75,.25,.25); green -> (.25,.75,.25); blue -> (.25,.25,.75); yellow -> (.75,.75,.25) */
	ok = is(16, 16, 191, 64, 64, why, 200) && is(48, 16, 64, 191, 64, why, 200) && is(16, 48, 64, 64, 191, why, 200) &&
	     is(48, 48, 191, 191, 64, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_fb_mix(char *why)
{
	/* the MBX draws green over everything, softpipe a smaller square, the MBX a still smaller one: each keeps what is under it */
	static const float g[] = { -1, -1, 0, 1, 0, 1, 1, -1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1,
				   -1, -1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, -1, 1, 0, 1, 0, 1 };
	static const float r[] = { -0.25f, -0.25f, 1, 0, 0, 1, 0.25f, -0.25f, 1, 0, 0, 1, 0.25f, 0.25f, 1, 0, 0, 1,
				   -0.25f, -0.25f, 1, 0, 0, 1, 0.25f, 0.25f, 1, 0, 0, 1, -0.25f, 0.25f, 1, 0, 0, 1 };
	begin();
	draw(GL_TRIANGLES, g, 6);			/* the MBX: green */
	glUseProgram(prog_fba);
	rect(-0.5f, -0.5f, 0.5f, 0.5f, 1, 1, 1);	/* softpipe: (1,1,1)*.5+.25 = (.75, .75, .75) in the middle half */
	glUseProgram(prog);
	draw(GL_TRIANGLES, r, 6);			/* the MBX again: red in the middle quarter */
	CHECK(4, 4, 0, 255, 0);				/* the first draw survived the CPU's */
	CHECK(20, 20, 191, 191, 191);			/* the CPU's draw is there */
	CHECK(32, 32, 255, 0, 0);			/* the last MBX draw is over it */
	return 1;
}

static int t_fb_depth(char *why)
{
	/* depth between two software draws: the nearer one wins whichever comes first */
	begin();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	{
		float a[3][7] = { { -0.9f, -0.9f, 0.2f, 1, 0, 0, 1 }, { 0.9f, -0.9f, 0.2f, 1, 0, 0, 1 }, { 0, 0.9f, 0.2f, 1, 0, 0, 1 } };
		float b[3][7] = { { -0.9f, -0.9f, 0.6f, 0, 1, 0, 1 }, { 0.9f, -0.9f, 0.6f, 0, 1, 0, 1 }, { 0, 0.9f, 0.6f, 0, 1, 0, 1 } };

		glUseProgram(prog_fbz);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), a);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), &a[0][3]);
		glDrawArrays(GL_TRIANGLES, 0, 3);	/* near, (1,0,0) -> (.75, .25, .25) */
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), b);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), &b[0][3]);
		glDrawArrays(GL_TRIANGLES, 0, 3);	/* far, (0,1,0) -> (.25,.75,.25): behind */
		glUseProgram(prog);
	}
	glDisable(GL_DEPTH_TEST);
	CHECK(32, 24, 191, 64, 64);
	return 1;
}

static int t_colormask(char *why)
{
	/* white drawn with only red and alpha enabled leaves green and blue as they were */
	begin();
	glClearColor(0, 0.5f, 0.25f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_TRUE);
	tri_z(0, 1, 1, 1, 1, 0);
	glColorMask(1, 1, 1, 1);
	CHECK(32, 24, 255, 128, 64);
	CHECK(2, 60, 0, 128, 64);
	return 1;
}

static int t_blend_constant(char *why)
{
	/* the source times the constant colour (0.5, 0.25, 1) over nothing: red (1, 0, 0) -> (128, 0, 0), white -> (128, 64, 255) */
	begin();
	glBlendColor(0.5f, 0.25f, 1.0f, 1.0f);
	glEnable(GL_BLEND);
	glBlendFunc(GL_CONSTANT_COLOR, GL_ZERO);
	tri_z(0, 1, 1, 1, 1, 0);
	glDisable(GL_BLEND);
	CHECK(32, 24, 128, 64, 255);
	return 1;
}

static int t_blend_subtract(char *why)
{
	/* destination minus source: (0.75, 0.5, 0.25) - (0.25, 0.25, 0.25) = (.5, .25, 0) */
	begin();
	glClearColor(0.75f, 0.5f, 0.25f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
	glBlendFunc(GL_ONE, GL_ONE);
	tri_z(0, 0.25f, 0.25f, 0.25f, 1, 0);
	glBlendEquation(GL_FUNC_ADD);
	glDisable(GL_BLEND);
	CHECK(32, 24, 128, 64, 0);
	return 1;
}

static int t_polygon_offset(char *why)
{
	/* two triangles at the same depth: the second fails GL_LESS, unless polygon offset pulls it forward */
	begin();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	tri_z(0.5f, 1, 0, 0, 1, 0);
	tri_z(0.5f, 0, 1, 0, 1, 0);
	CHECK(32, 24, 255, 0, 0);			/* equal depth: the red one stays */
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(0, -4);
	tri_z(0.5f, 0, 0, 1, 1, 0);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_DEPTH_TEST);
	CHECK(32, 24, 0, 0, 255);			/* offset toward the viewer: the blue one wins */
	return 1;
}

static int t_tex_two_units(char *why)
{
	/* two textures multiplied (unit 0 replace, unit 1 previous times texel: both on the chip): quadrants times mid grey */
	unsigned char px[8 * 8 * 4], grey[8 * 8 * 4];
	GLuint t0, t1;
	int ok;

	quadrants(px, 8, 8);
	memset(grey, 128, sizeof(grey));
	for (int i = 0; i < 64; i++)
		grey[4 * i + 3] = 255;
	begin();
	glActiveTexture(GL_TEXTURE0);
	t0 = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glActiveTexture(GL_TEXTURE1);
	t1 = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, grey, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_two);
	glUniform1i(glGetUniformLocation(prog_two, "u_a"), 0);
	glUniform1i(glGetUniformLocation(prog_two, "u_b"), 1);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(16, 16, 128, 0, 0, why, 200) && is(48, 16, 0, 128, 0, why, 200) && is(16, 48, 0, 0, 128, why, 200) &&
	     is(48, 48, 128, 128, 0, why, 200);
	glUseProgram(prog);
	glActiveTexture(GL_TEXTURE1);
	glDeleteTextures(1, &t1);
	glActiveTexture(GL_TEXTURE0);
	glDeleteTextures(1, &t0);
	return ok;
}

static int t_mipmap(char *why)
{
	/* a 16 x 16 checker of black and white texels, mipmaps generated: drawn minified (16 -> 4 pixels) with a trilinear
	 * filter, the texels average to grey; with a shader the chip cannot do, so the CPU samples the levels */
	unsigned char px[16 * 16 * 4];
	GLuint t;
	int ok = 1;
	struct px a, b;

	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++) {
			unsigned char c = ((x ^ y) & 1) ? 255 : 0;

			px[4 * (y * 16 + x)] = px[4 * (y * 16 + x) + 1] = px[4 * (y * 16 + x) + 2] = c;
			px[4 * (y * 16 + x) + 3] = 255;
		}
	begin();
	t = make_tex(16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_LINEAR_MIPMAP_NEAREST, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glGenerateMipmap(GL_TEXTURE_2D);
	glUseProgram(prog_fbt);
	glUniform1i(glGetUniformLocation(prog_fbt, "u_tex"), 0);
	rect(-1, -1, -0.875f, -0.875f, 1, 1, 1);	/* 4 x 4 pixels at the bottom left */
	/* fs_tex_arith: texel * .5 + .25: a grey texel g gives g * .5 + .25 (checker would give .25 and .75) */
	a = probe(1, 1);
	b = probe(2, 2);
	if (abs(a.c[0] - b.c[0]) > 24 || a.c[0] < 100 || a.c[0] > 156) {
		snprintf(why, 200, "minified checker not averaged: (1,1) %d, (2,2) %d (a mid-grey 128 expected)", a.c[0], b.c[0]);
		ok = 0;
	}
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_copy_tex(char *why)
{
	/* copy the framebuffer's lower left 8 x 8 (green) into a texture, clear, draw the texture: green again */
	static const float g[] = { -1, -1, 0, 1, 0, 1, 1, -1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1,
				   -1, -1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, -1, 1, 0, 1, 0, 1 };
	GLuint t;
	int ok;

	begin();
	draw(GL_TRIANGLES, g, 6);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 8, 8, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	begin();
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(32, 32, 0, 255, 0, why, 200) && is(5, 60, 0, 255, 0, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_subimage(char *why)
{
	/* an 8 x 8 red texture with a 4 x 4 green block written over its lower left quarter */
	unsigned char red[8 * 8 * 4], green[4 * 4 * 4];
	GLuint t;
	int ok;

	for (int i = 0; i < 64; i++) {
		red[4 * i] = 255; red[4 * i + 1] = 0; red[4 * i + 2] = 0; red[4 * i + 3] = 255;
	}
	for (int i = 0; i < 16; i++) {
		green[4 * i] = 0; green[4 * i + 1] = 255; green[4 * i + 2] = 0; green[4 * i + 3] = 255;
	}
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, red, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, green);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	quad(0, 0, 1, 1, 1, 1, 1);
	ok = is(16, 16, 0, 255, 0, why, 200) && is(48, 16, 255, 0, 0, why, 200) && is(16, 48, 255, 0, 0, why, 200) &&
	     is(48, 48, 255, 0, 0, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_render_to_texture(char *why)
{
	/* an orange clear into a 32 x 32 texture through its own framebuffer, then the texture drawn into the main one */
	GLuint t, f;
	int ok = 1;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenFramebuffers(1, &f);
	glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		snprintf(why, 200, "the texture framebuffer is incomplete");
		ok = 0;
	} else {
		glViewport(0, 0, 32, 32);
		glClearColor(1, 0.5f, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
		glViewport(0, 0, W, H);
		begin();
		glBindTexture(GL_TEXTURE_2D, t);
		glUseProgram(prog_tex);
		glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
		quad(0, 0, 1, 1, 1, 1, 1);
		ok = is(32, 32, 255, 128, 0, why, 200) && is(3, 3, 255, 128, 0, why, 200);
		glUseProgram(prog);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
	glViewport(0, 0, W, H);
	glDeleteFramebuffers(1, &f);
	glDeleteTextures(1, &t);
	return ok;
}

/* a triangle into render targets of growing size. The chip's address space is 32 MB a process, so a 2048 x 2048 target (16 MB, and as much again for its depth memory) cannot be made: it stops at 1536 (dEQP's default 2048 target needs --deqp-surface-width/height) */
static int t_big_target(char *why)
{
	static const int sizes[] = { 256, 512, 1024, 1536 };
	static const float v[] = { -0.6f, -0.6f, 0, 1, 1, 1, 1,  0.6f, -0.6f, 0, 1, 1, 1, 1,  0, 0.6f, 0, 1, 1, 1, 1 };
	int ok = 1;

	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && ok; i++) {
		int n = sizes[i];
		GLuint t, f;

		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glGenFramebuffers(1, &f);
		glBindFramebuffer(GL_FRAMEBUFFER, f);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			snprintf(why, 200, "a %d x %d framebuffer is incomplete", n, n);
			ok = 0;
		} else {
			glViewport(0, 0, n, n);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			draw_z(GL_TRIANGLES, v, 3);
			if (!is(n / 2, n / 2, 255, 255, 255, why, 200) || !is(2, 2, 0, 0, 0, why, 200)) {
				char tmp[200];

				snprintf(tmp, sizeof(tmp), "%d x %d target: %s", n, n, why);
				memcpy(why, tmp, sizeof(tmp));
				ok = 0;
			}
		}
		glBindFramebuffer(GL_FRAMEBUFFER, main_fbo);
		glViewport(0, 0, W, H);
		glDeleteFramebuffers(1, &f);
		glDeleteTextures(1, &t);
	}
	return ok;
}

/* dEQP's vertex-array shader: the colour is a scalar uniform times a constant vec3 (the other lanes of the uniform's slot are zero) */
static int t_vs_scalar_colour(char *why)
{
	static const char vs[] =
		"attribute highp vec2 a_0;\nuniform highp float u_coordScale;\nuniform highp float u_colorScale;\nvarying mediump vec4 v_color;\n"
		"void main(void) {\n	gl_PointSize = 1.0;\n	highp vec2 coord = vec2(1.0, 1.0);\n	highp vec3 color = vec3(1.0, 1.0, 1.0);\n"
		"	coord = a_0.xy;\n	v_color = vec4(u_colorScale * color, 1.0);\n	gl_Position = vec4(u_coordScale * coord, 1.0, 1.0);\n}\n";
	static const float v[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f };
	GLuint p = glCreateProgram();
	int ok;

	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(p, 0, "a_0");
	glLinkProgram(p);
	glUseProgram(p);
	begin();
	glUniform1f(glGetUniformLocation(p, "u_coordScale"), 1.0f);
	glUniform1f(glGetUniformLocation(p, "u_colorScale"), 1.0f);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	ok = is(32, 28, 255, 255, 255, why, 200);
	glUseProgram(prog);
	glDeleteProgram(p);
	return ok;
}

static int t_mip_chip(char *why)
{
	/* mipmaps on the chip (a shader it can do): a 32 x 32 texture whose levels are flat colours (level 0 red, 1 green, 2 blue,
	 * 3 yellow, 4 cyan, 5 magenta), drawn magnified (level 0) and minified 4 times (level 1) and 8 times (level 2) */
	static const unsigned char col[6][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
						 { 255, 255, 0, 255 }, { 0, 255, 255, 255 }, { 255, 0, 255, 255 } };
	unsigned char *lvl;
	GLuint t;
	int ok = 1;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	for (int l = 0; l <= 5; l++) {
		int n = 32 >> l;

		lvl = malloc((size_t)n * n * 4);
		for (int i = 0; i < n * n; i++)
			memcpy(lvl + 4 * i, col[l], 4);
		glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, lvl);
		free(lvl);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	begin();
	quad(0, 0, 1, 1, 1, 1, 1);		/* 32 texels over 64 pixels: level 0 */
	if (!is(32, 32, 255, 0, 0, why, 200))
		ok = 0;
	begin();
	quad(0, 0, 4, 4, 1, 1, 1);		/* 128 texels over 64 pixels: 2 per pixel, level 1 */
	if (ok && !is(32, 32, 0, 255, 0, why, 200))
		ok = 0;
	begin();
	quad(0, 0, 8, 8, 1, 1, 1);		/* 4 per pixel: level 2 */
	if (ok && !is(32, 32, 0, 0, 255, why, 200))
		ok = 0;
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

/* trilinear on the chip (README assumption 47): the same flat-colour chain, 1.5 texels per pixel is a blend of level 0 (red) and level 1 (green) */
static int t_trilinear(char *why)
{
	static const unsigned char col[6][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
						 { 255, 255, 0, 255 }, { 0, 255, 255, 255 }, { 255, 0, 255, 255 } };
	GLuint t;
	struct px a;
	int ok = 1;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	for (int l = 0; l <= 5; l++) {
		int n = 32 >> l;
		unsigned char *lvl = malloc((size_t)n * n * 4);

		for (int i = 0; i < n * n; i++)
			memcpy(lvl + 4 * i, col[l], 4);
		glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, lvl);
		free(lvl);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glUseProgram(prog_tex);
	glUniform1i(glGetUniformLocation(prog_tex, "u_tex"), 0);
	begin();
	quad(0, 0, 3, 3, 1, 1, 1);		/* 96 texels over 64 pixels */
	a = probe(32, 32);
	if (a.c[0] < 90 || a.c[0] > 170 || a.c[1] < 90 || a.c[1] > 170 || a.c[2] > 20) {
		snprintf(why, 200, "(32,32) is %d %d %d, expected a red-green blend", a.c[0], a.c[1], a.c[2]);
		ok = 0;
	}
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_bilinear_black(char *why)
{
	/* a black-to-white edge filtered: the chip skips the filter next to an all-zero texel, so the driver writes black as
	 * colour 1 and the seam is a ramp */
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok = 1;
	struct px a;

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
	a = probe(32, 32);
	if (a.c[0] < 60 || a.c[0] > 200) {
		snprintf(why, 200, "the seam (32,32) is %d: a mixed value expected between black and white", a.c[0]);
		ok = 0;
	}
	if (ok && probe(10, 32).c[0] > 8) {
		snprintf(why, 200, "the black side is %d", probe(10, 32).c[0]);
		ok = 0;
	}
	glDeleteTextures(1, &t);
	glUseProgram(prog);
	return ok;
}

static int t_tex_sum(char *why)
{
	/* texel * colour + a uniform: quadrants * .5 + (.25, 0, .25) */
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok;

	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_sum);
	glUniform1i(glGetUniformLocation(prog_sum, "u_tex"), 0);
	glUniform4f(glGetUniformLocation(prog_sum, "u_add"), 0.25f, 0.0f, 0.25f, 0.0f);
	quad(0, 0, 1, 1, 0.5f, 0.5f, 0.5f);
	/* red (1,0,0) -> (.75, 0, .25); green -> (.25, .5, .25); blue -> (.25, 0, .75); yellow -> (.75, .5, .25) */
	ok = is(16, 16, 191, 0, 64, why, 200) && is(48, 16, 64, 128, 64, why, 200) && is(16, 48, 64, 0, 191, why, 200) &&
	     is(48, 48, 191, 128, 64, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_mix(char *why)
{
	/* mix(texel, colour, .25): .75 * texel + .25 * (.2, .4, .8) */
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok;

	quadrants(px, 8, 8);
	begin();
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_mix);
	glUniform1i(glGetUniformLocation(prog_mix, "u_tex"), 0);
	glUniform4f(glGetUniformLocation(prog_mix, "u_color"), 0.2f, 0.4f, 0.8f, 1.0f);
	quad(0, 0, 1, 1, 1, 1, 1);
	/* red -> (.75 + .05, .1, .2) = (204, 26, 51); green -> (.05, .85, .2); blue -> (.05, .1, .95); yellow -> (.8, .85, .2) */
	ok = is(16, 16, 204, 26, 51, why, 200) && is(48, 16, 13, 217, 51, why, 200) && is(16, 48, 13, 26, 242, why, 200) &&
	     is(48, 48, 204, 217, 51, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_alpha_only(char *why)
{
	/* the colour from the vertices, the alpha from a texture (a glyph atlas): blended over green, the left half of the
	 * texture transparent, the right half opaque */
	unsigned char px[8 * 8 * 4];
	GLuint t;
	int ok;

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			px[4 * (y * 8 + x)] = px[4 * (y * 8 + x) + 1] = px[4 * (y * 8 + x) + 2] = 255;
			px[4 * (y * 8 + x) + 3] = x >= 4 ? 255 : 0;
		}
	begin();
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	t = make_tex(8, 8, GL_RGBA, GL_UNSIGNED_BYTE, px, GL_NEAREST, GL_CLAMP_TO_EDGE);
	glUseProgram(prog_txt);
	glUniform1i(glGetUniformLocation(prog_txt, "u_tex"), 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	quad(0, 0, 1, 1, 1, 0, 0);	/* red text colour */
	glDisable(GL_BLEND);
	ok = is(10, 32, 0, 255, 0, why, 200) && is(50, 32, 255, 0, 0, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static GLuint make_tex1(int w, int h, GLenum fmt, const unsigned char *data, GLint filter)
{
	GLuint t;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

static int t_tex_a8(char *why)
{
	/* a one-byte alpha atlas (GL_ALPHA): the left half 0, the right half 255, drawn as text colour red over green */
	unsigned char px[8 * 8];
	GLuint t;
	int ok;

	for (int i = 0; i < 64; i++)
		px[i] = (i % 8) >= 4 ? 255 : 0;
	begin();
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	t = make_tex1(8, 8, GL_ALPHA, px, GL_LINEAR);
	glUseProgram(prog_txt);
	glUniform1i(glGetUniformLocation(prog_txt, "u_tex"), 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	quad(0, 0, 1, 1, 1, 0, 0);
	glDisable(GL_BLEND);
	/* the seam is a ramp (bilinear next to a zero byte: the driver stores 1 for 0), the sides are pure */
	ok = is(8, 32, 0, 255, 0, why, 200) && is(56, 32, 255, 0, 0, why, 200);
	if (ok && (probe(32, 32).c[0] < 40 || probe(32, 32).c[0] > 215)) {
		snprintf(why, 200, "the A8 seam (32,32) is %d: a ramp expected", probe(32, 32).c[0]);
		ok = 0;
	}
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static int t_tex_l8(char *why)
{
	/* a luminance texture (GL_LUMINANCE): 128 on the left, 255 on the right, times a white vertex colour: grey, with alpha 1
	 * (blended over green it must not mix) */
	unsigned char px[8 * 8];
	GLuint t;
	int ok;

	for (int i = 0; i < 64; i++)
		px[i] = (i % 8) >= 4 ? 255 : 128;
	begin();
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	t = make_tex1(8, 8, GL_LUMINANCE, px, GL_NEAREST);
	glUseProgram(prog_mod);
	glUniform1i(glGetUniformLocation(prog_mod, "u_tex"), 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	quad(0, 0, 1, 1, 1, 1, 1);
	glDisable(GL_BLEND);
	ok = is(8, 32, 128, 128, 128, why, 200) && is(56, 32, 255, 255, 255, why, 200);
	glUseProgram(prog);
	glDeleteTextures(1, &t);
	return ok;
}

static const struct { const char *name; int (*fn)(char *); } tests[] = {
	{ "tex_a8", t_tex_a8 },
	{ "tex_l8", t_tex_l8 },
	{ "tex_sum", t_tex_sum },
	{ "tex_mix", t_tex_mix },
	{ "tex_alpha_only", t_tex_alpha_only },
	{ "bilinear_black", t_bilinear_black },
	{ "big_target", t_big_target },
	{ "vs_scalar_colour", t_vs_scalar_colour },
	{ "mip_chip", t_mip_chip },
	{ "trilinear", t_trilinear },
	{ "copy_tex", t_copy_tex },
	{ "tex_subimage", t_tex_subimage },
	{ "render_to_texture", t_render_to_texture },
	{ "mipmap", t_mipmap },
	{ "tex_two_units", t_tex_two_units },
	{ "colormask", t_colormask },
	{ "blend_constant", t_blend_constant },
	{ "blend_subtract", t_blend_subtract },
	{ "polygon_offset", t_polygon_offset },
	{ "fb_arith", t_fb_arith },
	{ "fb_tex", t_fb_tex },
	{ "fb_mix", t_fb_mix },
	{ "fb_depth", t_fb_depth },
	{ "blend_alpha", t_blend_alpha },
	{ "blend_add", t_blend_add },
	{ "cull", t_cull },
	{ "depth", t_depth },
	{ "depth_mask", t_depth_mask },
	{ "fs_uniform", t_fs_uniform },
	{ "fs_var_uniform", t_fs_var_uniform },
	{ "tex_uniform", t_tex_uniform },
	{ "tex_weston", t_tex_weston },
	{ "weston_vs", t_weston_vs },
	{ "scissor_draw", t_scissor_draw },
	{ "scissor_clear", t_scissor_clear },
	{ "clear_flat", t_clear_flat },
	{ "damage", t_damage },
	{ "stencil", t_stencil },
	{ "stencil_only", t_stencil_only },
	{ "stencil_depth", t_stencil_depth },
	{ "stencil_chip", t_stencil_chip },
	{ "depth_cpu_cpu", t_depth_cpu_cpu },
	{ "depth_chip_cpu", t_depth_chip_cpu },
	{ "depth_chip_cpu_chip", t_depth_chip_cpu_chip },
	{ "grid_light", t_grid_light },
	{ "grid_tex", t_grid_tex },
	{ "frag_depth", t_frag_depth },
	{ "alpha_discard", t_alpha_discard },
	{ "cube", t_cube },
	{ "tex_replace", t_tex_replace },
	{ "tex_rgb", t_tex_rgb },
	{ "dmabuf_reimport", t_dmabuf_reimport },
	{ "tex_modulate", t_tex_modulate },
	{ "tex_npot", t_tex_npot },
	{ "tex_bilinear", t_tex_bilinear },
	{ "tex_repeat", t_tex_repeat },
	{ "tex_565", t_tex_565 },
	{ "tex_4444", t_tex_4444 },
	{ "point", t_point },
	{ "point_sizes", t_point_sizes },
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
	main_fbo = fbo;
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
	prog_fba = link_tex(fs_arith);
	prog_fbt = link_tex(fs_tex_arith);
	prog_fbz = glCreateProgram();
	glAttachShader(prog_fbz, compile(GL_VERTEX_SHADER, zvs_src));
	glAttachShader(prog_fbz, compile(GL_FRAGMENT_SHADER, fs_arith));
	glBindAttribLocation(prog_fbz, 0, "a_pos");
	glBindAttribLocation(prog_fbz, 1, "a_color");
	glLinkProgram(prog_fbz);
	prog_two = link_tex(fs_two_tex);
	prog_sum = link_tex(fs_tex_sum);
	prog_mix = link_tex(fs_tex_mix);
	prog_txt = link_tex(fs_text);
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
