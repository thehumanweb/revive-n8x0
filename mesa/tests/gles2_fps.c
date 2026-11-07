// SPDX-License-Identifier: MIT
/*
 * Offscreen frame rates through the mbx driver: each scene is N frames of clear, draw, glFinish into an FBO (RGBA8
 * and a 16-bit depth), timed with CLOCK_MONOTONIC. Scenes: flat (one triangle), fill (a full-screen quad, plain
 * colour), tex (a full-screen textured quad: replace), mod (modulate), many (400 small triangles), cpu (a full-screen
 * quad with a shader the chip cannot do: the CPU fallback).
 *
 *   gles2_fps [-w width] [-h height] [-n frames] [scene ...]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *vs_src =
	"attribute vec2 a_pos;\nattribute vec2 a_uv;\nattribute vec4 a_color;\nvarying vec2 v_uv;\nvarying vec4 v_color;\n"
	"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; v_color = a_color; }\n";
static const char *fs_flat = "precision mediump float;\nvarying vec4 v_color;\nvoid main() { gl_FragColor = v_color; }\n";
static const char *fs_tex = "precision mediump float;\nuniform sampler2D u_tex;\nvarying vec2 v_uv;\n"
	"void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
static const char *fs_mod = "precision mediump float;\nuniform sampler2D u_tex;\nvarying vec2 v_uv;\nvarying vec4 v_color;\n"
	"void main() { gl_FragColor = texture2D(u_tex, v_uv) * v_color; }\n";
static const char *fs_cpu = "precision mediump float;\nvarying vec4 v_color;\n"
	"void main() { if (v_color.r > 0.9) discard; gl_FragColor = vec4(v_color.rgb, 1.0); }\n";	/* a discard that is not on alpha: the CPU draws it */
static const char *fs_grid = "precision mediump float;\nvarying vec4 v_color;\n"
	"void main() { gl_FragColor = vec4(v_color.rgb * v_color.rgb, 1.0); }\n";	/* a varying squared: not affine, so the grid mode */

static GLuint link_prog(const char *fs)
{
	GLuint p = glCreateProgram(), v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(v, 1, &vs_src, NULL);
	glCompileShader(v);
	glShaderSource(f, 1, &fs, NULL);
	glCompileShader(f);
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_uv");
	glBindAttribLocation(p, 2, "a_color");
	glLinkProgram(p);
	return p;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void draw_verts(const float *v, int n)
{
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v + 2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), v + 4);
	glDrawArrays(GL_TRIANGLES, 0, n);
}

static const float quad[] = {
	-1, -1, 0, 0, 1, 0.5f, 0.25f, 1, 1, -1, 1, 0, 1, 0.5f, 0.25f, 1, 1, 1, 1, 1, 1, 0.5f, 0.25f, 1,
	-1, -1, 0, 0, 1, 0.5f, 0.25f, 1, 1, 1, 1, 1, 1, 0.5f, 0.25f, 1, -1, 1, 0, 1, 1, 0.5f, 0.25f, 1,
};

int main(int argc, char **argv)
{
	int w = 400, h = 240, frames = 30, o, i, f;
	static const char *all[] = { "flat", "fill", "tex", "mod", "many", "cpu", "grid" };
	PFNEGLGETPLATFORMDISPLAYEXTPROC gd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
	static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLConfig cfg;
	EGLint n;
	GLuint fbo, rb, db, tex, prog[5];
	unsigned char *tp;
	float *many;

	while ((o = getopt(argc, argv, "w:h:n:")) != -1) {
		if (o == 'w') w = atoi(optarg);
		else if (o == 'h') h = atoi(optarg);
		else if (o == 'n') frames = atoi(optarg);
		else return 2;
	}
	if (!eglInitialize(d, 0, 0) || !eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(d, ca, &cfg, 1, &n) || !n)
		return 2;
	if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa)))
		return 2;
	fprintf(stderr, "gles2_fps: %s, %dx%d, %d frames a scene\n", glGetString(GL_RENDERER), w, h, frames);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	glGenRenderbuffers(1, &db);
	glBindRenderbuffer(GL_RENDERBUFFER, db);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db);
	glViewport(0, 0, w, h);
	prog[0] = link_prog(fs_flat);
	prog[1] = link_prog(fs_tex);
	prog[2] = link_prog(fs_mod);
	prog[3] = link_prog(fs_cpu);
	prog[4] = link_prog(fs_grid);
	tp = malloc(256 * 256 * 4);
	for (i = 0; i < 256 * 256; i++) {
		tp[4 * i] = i & 255;
		tp[4 * i + 1] = (i >> 8) & 255;
		tp[4 * i + 2] = 128;
		tp[4 * i + 3] = 255;
	}
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, tp);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	many = malloc(400 * 3 * 8 * sizeof(float));
	for (i = 0; i < 400; i++) {
		float x = -0.95f + (i % 20) * 0.095f, y = -0.95f + (i / 20) * 0.095f;
		float *t = many + i * 24;
		const float tri[3][2] = { { 0, 0 }, { 0.07f, 0 }, { 0, 0.07f } };

		for (int k = 0; k < 3; k++) {
			t[8 * k] = x + tri[k][0]; t[8 * k + 1] = y + tri[k][1]; t[8 * k + 2] = 0; t[8 * k + 3] = 0;
			t[8 * k + 4] = 1; t[8 * k + 5] = 0.5f; t[8 * k + 6] = 0.25f; t[8 * k + 7] = 1;
		}
	}

	for (i = 0; i < 7; i++) {
		double t0 = 0, t1;
		int want = argc - optind == 0, nf = frames;

		for (f = optind; f < argc; f++)
			if (!strcmp(argv[f], all[i]))
				want = 1;
		if (!want)
			continue;
		if (i == 5)
			nf = frames > 6 ? 6 : frames;	/* the CPU is slow */
		for (f = -1; f < nf; f++) {		/* the first frame warms up (program, buffers) and is not timed */
			if (f == 0) {
				glFinish();
				t0 = now();
			}
			if (!getenv("FPS_NOCLEAR")) {
				glClearColor(0.1f, 0.1f, 0.2f, 1);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			}
			switch (i) {
			case 0: glUseProgram(prog[0]); draw_verts(quad, 3); break;
			case 1: glUseProgram(prog[0]); draw_verts(quad, 6); break;
			case 2: glUseProgram(prog[1]); glUniform1i(glGetUniformLocation(prog[1], "u_tex"), 0); draw_verts(quad, 6); break;
			case 3: glUseProgram(prog[2]); glUniform1i(glGetUniformLocation(prog[2], "u_tex"), 0); draw_verts(quad, 6); break;
			case 4: glUseProgram(prog[0]); draw_verts(many, 400 * 3); break;
			case 5: glUseProgram(prog[3]); draw_verts(quad, 6); break;
			case 6: glUseProgram(prog[4]); draw_verts(quad, 6); break;
			}
			glFinish();
		}
		t1 = now();
		printf("%-5s %7.1f ms/frame  %6.1f fps\n", all[i], (t1 - t0) * 1000 / nf, nf / (t1 - t0));
		fflush(stdout);
	}
	return 0;
}
