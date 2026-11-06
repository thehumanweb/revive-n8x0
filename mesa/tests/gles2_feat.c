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

static GLuint prog;
static int failures;

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

static const struct { const char *name; int (*fn)(char *); } tests[] = {
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
