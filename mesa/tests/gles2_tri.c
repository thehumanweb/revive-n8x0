// SPDX-License-Identifier: MIT
/*
 * The mbxtri triangle through GLES2: EGL (surfaceless), a 64x64 renderbuffer
 * framebuffer, a vertex shader with a uniform matrix, a varying colour, and
 * glReadPixels, written as a PPM. Run it on the host sim and compare with
 * mbx/mbxtri's picture (same angle): they should be identical.
 *
 *   gles2_tri [-a degrees] [-o out.ppm]
 *
 * The y flip is in the matrix, so the picture's rows are the emulator's
 * rows: window row 0 is the first row of the buffer, as in mbxtri.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 64
#define H 64

static const char *vs_src =
	"uniform mat4 u_mvp;\n"
	"attribute vec3 a_pos;\n"
	"attribute vec4 a_color;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"	gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"	v_color = a_color;\n"
	"}\n";

static const char *fs_src =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"	gl_FragColor = v_color;\n"
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
		exit(1);
	}
	return s;
}

#define CHECK_GL(what) do { GLenum e = glGetError(); if (e) { fprintf(stderr, "gles2_tri: %s: GL error 0x%x\n", what, e); return 1; } } while (0)

int main(int argc, char **argv)
{
	const char *out = "gles2_tri.ppm";
	float deg = 0;
	int o, i, x, y, lit = 0, blend = 0, reps = 1, rep;

	while ((o = getopt(argc, argv, "a:o:b:n:")) != -1) {
		if (o == 'a') deg = (float)atof(optarg);
		else if (o == 'n') reps = atoi(optarg);
		else if (o == 'o') out = optarg;
		else if (o == 'b') blend = atoi(optarg);
		else return 2;
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint major, minor;
	static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLConfig cfg;
	EGLint ncfg;
	EGLContext ctx;

	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "gles2_tri: no EGL display (0x%x)\n", eglGetError());
		return 1;
	}
	fprintf(stderr, "EGL %d.%d, vendor %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || !ncfg) {
		fprintf(stderr, "gles2_tri: no config\n");
		return 1;
	}
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "gles2_tri: no context (0x%x)\n", eglGetError());
		return 1;
	}
	fprintf(stderr, "GL_RENDERER %s\n", glGetString(GL_RENDERER));

	GLuint fbo, rb, db;

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
		fprintf(stderr, "gles2_tri: framebuffer incomplete (0x%x)\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
		return 1;
	}

	GLuint prog = glCreateProgram();
	GLint ok = 0;

	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog, 0, "a_pos");
	glBindAttribLocation(prog, 1, "a_color");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024];

		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		fprintf(stderr, "link: %s\n", log);
		return 1;
	}
	glUseProgram(prog);

	/* F * R: rotate about z by deg, then flip y (the picture's row 0 is the first row of the buffer) */
	float c = cosf(deg * (float)M_PI / 180), s = sinf(deg * (float)M_PI / 180);
	float m[16] = { c, -s, 0, 0,	/* rows of R, then y negated */
			-s, -c, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1 };
	float col[16];

	for (i = 0; i < 16; i++)
		col[(i % 4) * 4 + i / 4] = m[i];	/* to column-major */
	glUniformMatrix4fv(glGetUniformLocation(prog, "u_mvp"), 1, GL_FALSE, col);

	static const float verts[3][7] = {
		{ 0.0f, 0.8f, 0.5f, 1, 0, 0, 1 },
		{ -0.8f, -0.6f, 0.5f, 0, 1, 0, 1 },
		{ 0.8f, -0.6f, 0.5f, 0, 0, 1, 1 },
	};

	glViewport(0, 0, W, H);
	unsigned char *px = malloc(W * H * 4);

	for (rep = 0; rep < reps; rep++) {	/* -n: the same frame again, one job each */
	glClearColor(32 / 255.0f, 32 / 255.0f, 32 / 255.0f, 1.0f);
	glClearDepthf(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	if (blend == 1) {			/* additive: dst + src */
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
	} else if (blend == 2) {		/* modulate: src * dst */
		glEnable(GL_BLEND);
		glBlendFunc(GL_DST_COLOR, GL_ZERO);
	}
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), &verts[0][0]);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), &verts[0][3]);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	CHECK_GL("draw");

	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	CHECK_GL("readpixels");
	if (reps > 1) {
		int n = 0;

		for (i = 0; i < W * H; i++)
			n += (px[4 * i] != 32 || px[4 * i + 1] != 32 || px[4 * i + 2] != 32);
		fprintf(stderr, "gles2_tri: frame %d: %d pixels drawn\n", rep, n);
	}
	}

	FILE *f = fopen(out, "wb");

	if (!f)
		return 1;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			unsigned char *p = px + (y * W + x) * 4;

			fputc(p[0], f);
			fputc(p[1], f);
			fputc(p[2], f);
			lit += !(p[0] == 32 && p[1] == 32 && p[2] == 32);
		}
	fclose(f);
	fprintf(stderr, "gles2_tri: %d of %d pixels drawn, wrote %s\n", lit, W * H, out);
	return lit ? 0 : 1;
}
