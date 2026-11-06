// SPDX-License-Identifier: MIT
/*
 * An animated triangle on the display: EGL on GBM on the KMS device (omapdrm),
 * GLES2 drawing through the mbx driver (kmsro: the display allocates the
 * buffers, the MBX renders into them), and a KMS mode set and page flips.
 *
 *   gbm_tri [-d /dev/dri/card0] [-n frames] [-s degrees a frame] [-o last.ppm]
 *
 * -n 0 runs until killed. -o writes the last frame as a PPM through glReadPixels,
 * for checking without a screen.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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
		perror("gbm_tri: drmModeAddFB2");
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

int main(int argc, char **argv)
{
	const char *dev = "/dev/dri/card0", *out = NULL;
	int frames = 120, o, fd, i;
	float step = 3.0f;

	while ((o = getopt(argc, argv, "d:n:s:o:")) != -1) {
		if (o == 'd') dev = optarg;
		else if (o == 'n') frames = atoi(optarg);
		else if (o == 's') step = (float)atof(optarg);
		else if (o == 'o') out = optarg;
		else return 2;
	}

	if ((fd = open(dev, O_RDWR | O_CLOEXEC)) < 0) {
		perror("gbm_tri: open");
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
		fprintf(stderr, "gbm_tri: no connected connector with a mode\n");
		return 1;
	}
	drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : NULL;
	uint32_t crtc_id = enc ? enc->crtc_id : (res->count_crtcs ? res->crtcs[0] : 0);

	crtc = drmModeGetCrtc(fd, crtc_id);
	drmModeModeInfo mode = conn->modes[0];

	fprintf(stderr, "gbm_tri: %s, %dx%d@%d on connector %u, crtc %u\n", dev, mode.hdisplay, mode.vdisplay,
		mode.vrefresh, conn->connector_id, crtc_id);

	struct gbm_device *gbm = gbm_create_device(fd);
	struct gbm_surface *surf = gbm_surface_create(gbm, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888,
						      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

	if (!gbm || !surf) {
		fprintf(stderr, "gbm_tri: no GBM surface\n");
		return 1;
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	EGLint major, minor, ncfg;
	static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
					   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16,
					   EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLConfig cfgs[64], cfg = NULL;

	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "gbm_tri: no EGL display (0x%x)\n", eglGetError());
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(dpy, cfg_attr, cfgs, 64, &ncfg) || !ncfg) {
		fprintf(stderr, "gbm_tri: no EGL config\n");
		return 1;
	}
	for (i = 0; i < ncfg && !cfg; i++) {
		EGLint id;

		eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
		if ((uint32_t)id == GBM_FORMAT_XRGB8888)
			cfg = cfgs[i];
	}
	if (!cfg) {
		fprintf(stderr, "gbm_tri: no config for XRGB8888\n");
		return 1;
	}
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	EGLSurface esurf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)surf, NULL);

	if (ctx == EGL_NO_CONTEXT || esurf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, esurf, esurf, ctx)) {
		fprintf(stderr, "gbm_tri: EGL context or surface failed (0x%x)\n", eglGetError());
		return 1;
	}
	fprintf(stderr, "gbm_tri: GL_RENDERER %s\n", glGetString(GL_RENDERER));

	GLuint prog = glCreateProgram();
	GLint ok = 0;

	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog, 0, "a_pos");
	glBindAttribLocation(prog, 1, "a_color");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "gbm_tri: link failed\n");
		return 1;
	}
	glUseProgram(prog);
	GLint u_mvp = glGetUniformLocation(prog, "u_mvp");

	static const float verts[3][7] = {
		{ 0.0f, 0.8f, 0.5f, 1, 0, 0, 1 },
		{ -0.8f, -0.6f, 0.5f, 0, 1, 0, 1 },
		{ 0.8f, -0.6f, 0.5f, 0, 0, 1, 1 },
	};
	float aspect = (float)mode.vdisplay / (float)mode.hdisplay;

	glViewport(0, 0, mode.hdisplay, mode.vdisplay);
	glEnable(GL_DEPTH_TEST);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), &verts[0][0]);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), &verts[0][3]);

	drmEventContext evctx = { .version = 2, .page_flip_handler = page_flip_handler };
	struct gbm_bo *prev = NULL;
	int frame;

	for (frame = 0; !frames || frame < frames; frame++) {
		float a = frame * step * (float)M_PI / 180, c = cosf(a), s = sinf(a);
		/* rotate in the plane of the screen: scale x by the aspect first so the triangle keeps its shape */
		float m[16] = { c * aspect, -s * aspect, 0, 0,
				s, c, 0, 0,
				0, 0, 1, 0,
				0, 0, 0, 1 };
		float col[16];

		/* a rotation then the aspect fix: x' = aspect * (c x - s y), y' = s x + c y (column-major) */
		for (i = 0; i < 16; i++)
			col[(i % 4) * 4 + i / 4] = m[i];
		glUniformMatrix4fv(u_mvp, 1, GL_FALSE, col);
		glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		if (out && frame == frames - 1) {
			unsigned char *px = malloc(mode.hdisplay * mode.vdisplay * 4);
			FILE *f = fopen(out, "wb");

			glReadPixels(0, 0, mode.hdisplay, mode.vdisplay, GL_RGBA, GL_UNSIGNED_BYTE, px);
			if (f) {
				fprintf(f, "P6\n%d %d\n255\n", mode.hdisplay, mode.vdisplay);
				for (int y = mode.vdisplay - 1; y >= 0; y--)	/* GL's row 0 is the bottom */
					for (int x = 0; x < mode.hdisplay; x++)
						fwrite(px + (y * mode.hdisplay + x) * 4, 1, 3, f);
				fclose(f);
			}
			free(px);
		}

		eglSwapBuffers(dpy, esurf);
		struct gbm_bo *bo = gbm_surface_lock_front_buffer(surf);
		struct fb *fb = bo ? fb_for(bo) : NULL;

		if (!fb) {
			fprintf(stderr, "gbm_tri: no front buffer for frame %d\n", frame);
			return 1;
		}
		if (!prev) {
			if (drmModeSetCrtc(fd, crtc_id, fb->id, 0, 0, &conn->connector_id, 1, &mode)) {
				perror("gbm_tri: drmModeSetCrtc");
				return 1;
			}
		} else {
			flipped = 0;
			if (drmModePageFlip(fd, crtc_id, fb->id, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
				/* a display that cannot flip (a manual update panel): set the mode again */
				if (drmModeSetCrtc(fd, crtc_id, fb->id, 0, 0, &conn->connector_id, 1, &mode)) {
					perror("gbm_tri: drmModeSetCrtc");
					return 1;
				}
			} else {
				struct pollfd p = { .fd = fd, .events = POLLIN };

				while (!flipped && poll(&p, 1, 1000) > 0)
					drmHandleEvent(fd, &evctx);
				if (!flipped)
					fprintf(stderr, "gbm_tri: frame %d: no flip event\n", frame);
			}
			gbm_surface_release_buffer(surf, prev);
		}
		prev = bo;
	}
	fprintf(stderr, "gbm_tri: %d frames\n", frame);
	if (crtc)
		drmModeSetCrtc(fd, crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y, &conn->connector_id, 1, &crtc->mode);
	return 0;
}
