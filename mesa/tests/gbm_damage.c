// SPDX-License-Identifier: MIT
/*
 * Partial updates end to end: EGL on GBM on the KMS device (omapdrm), a 64 x 64 square moving over a static background, page flips with
 * no damage clips of their own.
 *
 *   gbm_damage [-d /dev/dri/card0] [-n frames] [-D 0|1] [-s pixels a frame] [-v]
 *
 * -D 0 redraws the whole frame every time (damage unknown: the whole frame goes to the panel). -D 1 asks for the buffer's age
 * (EGL_EXT_buffer_age), says what changed since then with eglSetDamageRegionKHR and draws only there (EGL_KHR_partial_update): the MBX
 * renders only those tiles, and the driver tells omapdrm what it drew, so the flip sends that rectangle (kernel patch 0065).
 * It prints the time per frame (a flip waits for the panel's update) and, with -v, checks the last frame against what it should hold.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define SQ 64

static const char *vs_src =
	"attribute vec2 a_pos;\n"
	"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_src =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"void main() { gl_FragColor = u_color; }\n";

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

struct fb {
	struct gbm_bo *bo;
	uint32_t id;
};

static void fb_destroy(struct gbm_bo *bo, void *data)
{
	struct fb *fb = data;

	if (fb->id)
		drmModeRmFB(gbm_device_get_fd(gbm_bo_get_device(bo)), fb->id);
	free(fb);
}

static struct fb *fb_for(struct gbm_bo *bo)
{
	struct fb *fb = gbm_bo_get_user_data(bo);
	int fd = gbm_device_get_fd(gbm_bo_get_device(bo));
	uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 }, strides[4] = { gbm_bo_get_stride(bo) }, offsets[4] = { 0 };

	if (fb)
		return fb;
	fb = calloc(1, sizeof(*fb));
	fb->bo = bo;
	if (drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, strides,
			  offsets, &fb->id, 0)) {
		perror("gbm_damage: drmModeAddFB2");
		free(fb);
		return NULL;
	}
	gbm_bo_set_user_data(bo, fb, fb_destroy);
	return fb;
}

static int flipped;

static void page_flip_handler(int fd, unsigned frame, unsigned sec, unsigned usec, void *data)
{
	flipped = 1;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static int sq_x(int frame, int step, int w)
{
	int span = w - SQ;
	int p = (frame * step) % (2 * span);

	return p < span ? p : 2 * span - p;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/dri/card0";
	int frames = 120, o, fd, i, partial = 1, step = 8, verify = 0;

	while ((o = getopt(argc, argv, "d:n:D:s:v")) != -1) {
		if (o == 'd') dev = optarg;
		else if (o == 'n') frames = atoi(optarg);
		else if (o == 'D') partial = atoi(optarg);
		else if (o == 's') step = atoi(optarg);
		else if (o == 'v') verify = 1;
		else return 2;
	}
	if ((fd = open(dev, O_RDWR | O_CLOEXEC)) < 0) {
		perror("gbm_damage: open");
		return 1;
	}
	drmModeRes *res = drmModeGetResources(fd);
	drmModeConnector *conn = NULL;
	drmModeCrtc *crtc = NULL;

	for (i = 0; res && i < res->count_connectors && !conn; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn && (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes)) {
			drmModeFreeConnector(conn);
			conn = NULL;
		}
	}
	if (!conn) {
		fprintf(stderr, "gbm_damage: no connected connector with a mode\n");
		return 1;
	}
	drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : NULL;
	uint32_t crtc_id = enc ? enc->crtc_id : (res->count_crtcs ? res->crtcs[0] : 0);
	drmModeModeInfo mode = conn->modes[0];
	int W = mode.hdisplay, H = mode.vdisplay;

	crtc = drmModeGetCrtc(fd, crtc_id);
	fprintf(stderr, "gbm_damage: %s, %dx%d@%d, %s\n", dev, W, H, mode.vrefresh, partial ? "partial updates" : "whole frames");

	struct gbm_device *gbm = gbm_create_device(fd);
	struct gbm_surface *surf = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

	if (!gbm || !surf) {
		fprintf(stderr, "gbm_damage: no GBM surface\n");
		return 1;
	}
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	EGLint major, minor, ncfg;
	static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
					   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLConfig cfgs[64], cfg = NULL;

	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "gbm_damage: no EGL display (0x%x)\n", eglGetError());
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
	PFNEGLSETDAMAGEREGIONKHRPROC set_damage = (PFNEGLSETDAMAGEREGIONKHRPROC)eglGetProcAddress("eglSetDamageRegionKHR");

	if (partial && (!strstr(exts, "EGL_KHR_partial_update") || !strstr(exts, "EGL_EXT_buffer_age") || !set_damage)) {
		fprintf(stderr, "gbm_damage: EGL_KHR_partial_update or EGL_EXT_buffer_age missing\n");
		return 1;
	}
	if (!eglChooseConfig(dpy, cfg_attr, cfgs, 64, &ncfg) || !ncfg) {
		fprintf(stderr, "gbm_damage: no EGL config\n");
		return 1;
	}
	for (i = 0; i < ncfg && !cfg; i++) {
		EGLint id;

		eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
		if ((uint32_t)id == GBM_FORMAT_XRGB8888)
			cfg = cfgs[i];
	}
	if (!cfg) {
		fprintf(stderr, "gbm_damage: no config for XRGB8888\n");
		return 1;
	}
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	EGLSurface esurf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)surf, NULL);

	if (ctx == EGL_NO_CONTEXT || esurf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, esurf, esurf, ctx)) {
		fprintf(stderr, "gbm_damage: EGL context or surface failed (0x%x)\n", eglGetError());
		return 1;
	}
	fprintf(stderr, "gbm_damage: GL_RENDERER %s\n", glGetString(GL_RENDERER));

	GLuint prog = glCreateProgram();

	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog, 0, "a_pos");
	glLinkProgram(prog);
	glUseProgram(prog);
	GLint u_color = glGetUniformLocation(prog, "u_color");

	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);

	drmEventContext evctx = { .version = 2, .page_flip_handler = page_flip_handler };
	struct gbm_bo *prev = NULL;
	double t0 = 0;
	int frame, last_x = 0, setcrtc_fallbacks = 0;
	double t_render = 0, t_swap = 0, t_flip = 0, tt;

	for (frame = 0; frame < frames; frame++) {
		int x = sq_x(frame, step, W), y = (H - SQ) / 2, age = 0;
		int dx0 = 0, dy0 = 0, dx1 = W, dy1 = H;		/* the rectangle drawn this frame, bottom-left origin */

		last_x = x;
		if (partial) {
			eglQuerySurface(dpy, esurf, EGL_BUFFER_AGE_EXT, &age);
			if (age > 0 && age <= 8 && frame >= age) {
				/* the buffer holds the frame age frames ago: what differs is the square then and now */
				int ox = sq_x(frame - age, step, W);

				dx0 = ox < x ? ox : x;
				dx1 = (ox > x ? ox : x) + SQ;
				dy0 = y;
				dy1 = y + SQ;
				EGLint r[4] = { dx0, dy0, dx1 - dx0, dy1 - dy0 };

				set_damage(dpy, esurf, r, 1);
			}		/* else nothing is known (age 0, or the first frames): not calling it means the whole buffer */
		}
		glEnable(GL_SCISSOR_TEST);
		glScissor(dx0, dy0, dx1 - dx0, dy1 - dy0);
		glUniform4f(u_color, 0.1f, 0.1f, 0.15f, 1.0f);
		glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		{
			float x0 = 2.0f * x / W - 1.0f, x1 = 2.0f * (x + SQ) / W - 1.0f, y0 = 2.0f * y / H - 1.0f, y1 = 2.0f * (y + SQ) / H - 1.0f;
			const float v[] = { x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1 };

			glUniform4f(u_color, 1.0f, 0.5f, 0.0f, 1.0f);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}
		glDisable(GL_SCISSOR_TEST);

		if (verify && frame == frames - 1) {
			unsigned char *px = malloc((size_t)W * H * 4);
			long bad = 0;

			glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
			for (int py = 0; py < H; py++)
				for (int pxl = 0; pxl < W; pxl++) {
					const unsigned char *p = px + ((size_t)py * W + pxl) * 4;
					int in = pxl >= last_x && pxl < last_x + SQ && py >= y && py < y + SQ;
					int ok = in ? (p[0] > 240 && p[1] > 118 && p[1] < 138 && p[2] < 12) : (p[0] > 20 && p[0] < 32 && p[2] > 34 && p[2] < 44);

					bad += !ok;
				}
			fprintf(stderr, "gbm_damage: last frame: %ld of %d pixels wrong\n", bad, W * H);
			free(px);
		}

		tt = now();
		glFinish();
		if (frame >= 10)
			t_render += now() - tt;
		tt = now();
		eglSwapBuffers(dpy, esurf);
		if (frame >= 10)
			t_swap += now() - tt;
		struct gbm_bo *bo = gbm_surface_lock_front_buffer(surf);
		struct fb *fb = bo ? fb_for(bo) : NULL;

		if (!fb) {
			fprintf(stderr, "gbm_damage: no front buffer for frame %d\n", frame);
			return 1;
		}
		if (!prev) {
			if (drmModeSetCrtc(fd, crtc_id, fb->id, 0, 0, &conn->connector_id, 1, &mode)) {
				perror("gbm_damage: drmModeSetCrtc");
				return 1;
			}
		} else {
			flipped = 0;
			tt = now();
			if (drmModePageFlip(fd, crtc_id, fb->id, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
				setcrtc_fallbacks++;
				if (drmModeSetCrtc(fd, crtc_id, fb->id, 0, 0, &conn->connector_id, 1, &mode)) {
					perror("gbm_damage: drmModeSetCrtc");
					return 1;
				}
			} else {
				struct pollfd p = { .fd = fd, .events = POLLIN };

				while (!flipped && poll(&p, 1, 1000) > 0)
					drmHandleEvent(fd, &evctx);
				if (!flipped)
					fprintf(stderr, "gbm_damage: frame %d: no flip event\n", frame);
			}
			if (frame >= 10)
				t_flip += now() - tt;
			gbm_surface_release_buffer(surf, prev);
		}
		prev = bo;
		if (frame == 9)
			t0 = now();	/* the first frames warm things up */
	}
	if (setcrtc_fallbacks)
		fprintf(stderr, "gbm_damage: %d page flips failed and fell back to a mode set\n", setcrtc_fallbacks);
	if (frames > 10) {
		double t = now() - t0;

		fprintf(stderr, "gbm_damage: a frame: %.1f ms glFinish, %.1f ms swap, %.1f ms flip\n", t_render * 1000 / (frames - 10),
			t_swap * 1000 / (frames - 10), t_flip * 1000 / (frames - 10));

		fprintf(stderr, "gbm_damage: %d frames in %.2f s: %.1f ms a frame, %.1f fps\n", frames - 10, t, t * 1000 / (frames - 10),
			(frames - 10) / t);
	}
	if (crtc)
		drmModeSetCrtc(fd, crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y, &conn->connector_id, 1, &crtc->mode);
	return 0;
}
