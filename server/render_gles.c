/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The GPU renderer (E38): GLES2 through EGL on GBM, on the DRM output's
 * own device. On a Raspberry Pi 5 that's vc4, and Mesa's kmsro renders
 * on v3d; on other machines whatever Mesa (or a vendor's GBM) drives.
 *
 * Each frame is drawn into a GBM surface the size of the panel; when it
 * ends, the DRM output flips to that buffer (gles_scanout) and the
 * buffer before it goes back to GBM once the flip is done
 * (gles_presented). Nothing here reads pixels back: srv->out stays NULL,
 * and captures and snapshots need the pixman renderer for now (T8).
 *
 * gles_open() runs as root, before privileges are dropped, because
 * eglInitialize is where Mesa opens the render node.
 *
 * Each window has its own texture, and a commit uploads only what it
 * damaged (gles_window_commit marks it, the next draw uploads it). Other
 * images (tiles, the cursor, drag images, overview previews) are still
 * uploaded whole each time they're drawn (T6).
 *
 * With EGL_EXT_buffer_age a frame repaints only what changed since the
 * GBM buffer it draws into was last on screen; without it, everything.
 *
 * GPU priority (#74): with EGL_IMG_context_priority the context asks to
 * be HIGH, so the composite jobs go ahead of a GPU-bound client's queued
 * work instead of waiting their turn behind it, and the desktop keeps its
 * frame rate while an app saturates the GPU. The driver decides: on v3d
 * that needs NextBSD's kernel extension and an fd opened as root, which
 * Mesa's is (gles_open). What was granted is logged; without the
 * extension nothing changes.
 *
 * GPU buffers (T12): a client's dmabuf is imported as an EGLImage and
 * drawn straight from it, never copied. The formats offered list the
 * GPU's own tiled modifiers before LINEAR, which v3d samples several
 * times slower. CPU users of such a window's pixels (tiles, previews,
 * window captures) get them read back on the GPU (gles_buffer_read).
 *
 * Still to come (nextbsd/nextbsd#642): a backing store of another size
 * than the panel (T7) and readback of the screen (T8).
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "server.h"

#ifndef GL_UNPACK_ROW_LENGTH_EXT
#define GL_UNPACK_ROW_LENGTH_EXT	0x0CF2
#define GL_UNPACK_SKIP_ROWS_EXT		0x0CF3
#define GL_UNPACK_SKIP_PIXELS_EXT	0x0CF4
#endif
#ifndef EGL_BUFFER_AGE_EXT
#define EGL_BUFFER_AGE_EXT		0x313D
#endif
#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG	0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG	0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG	0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG	0x3103
#endif

/* Damage of the frames before this one, newest first (buffer age). */
#define GLES_HISTORY	4

/* A GPU buffer: the client's dmabuf, as a texture. */
struct gles_buffer {
	EGLImageKHR	image;
	GLuint		tex;
};

/* A window's texture: its buffer's pixels, as of `dirty` (window-local
   points) being uploaded. */
struct gles_window {
	GLuint		tex;
	int		tw, th;
	pixman_format_code_t format;	/* of what was uploaded */
	int		scale;
	bool		all;		/* upload everything */
	pixman_region32_t dirty;
};

struct gles {
	struct gbm_device *gbm;
	struct gbm_surface *surface;
	EGLDisplay	dpy;
	EGLConfig	config;
	EGLContext	ctx;
	EGLSurface	egl_surface;
	uint32_t	width, height;	/* the surface: the panel */
	GLint		max_tex;	/* GL_MAX_TEXTURE_SIZE */
	bool		bgra;		/* GL_EXT_texture_format_BGRA8888 */
	bool		unpack_row;	/* GL_EXT_unpack_subimage */
	bool		buffer_age;	/* EGL_EXT_buffer_age */
	const char	*priority;	/* GPU priority granted, or NULL */
	/* GPU buffers: EGL_EXT_image_dma_buf_import, GL_OES_EGL_image */
	bool		dmabuf;
	char		render_node[32];
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
	pixman_region32_t history[GLES_HISTORY];	/* points */
	int		nhistory;
	GLuint		prog_tex, prog_fill;
	GLint		tex_screen, tex_opacity, tex_opaque, tex_swizzle;
	GLint		fill_screen, fill_color;
	GLuint		tex;		/* reused for every image (T3) */
	pixman_region32_t clip;		/* begin..end, backing pixels */
	/*
	 * WS_GL_VERIFY: a second copy of the screen, kept the slow sure
	 * way (only each frame's own damage drawn, every image uploaded
	 * whole), compared with each frame before it is shown.
	 */
	bool		verify;
	GLuint		mirror_fbo, mirror_tex;
	/* the GBM buffers can't be read back (Mesa can't map a scanout
	   buffer here), so they are simulated: one texture each, drawn
	   exactly as the frame drawn into that buffer */
#define GLES_SIM 4
	GLuint		sim_fbo[GLES_SIM], sim_tex[GLES_SIM];
	int		sim_of[64];	/* frame % 64 -> its sim buffer */
	int		sim_cur;
	pixman_region32_t frame_damage;	/* points, before buffer age */
	pixman_region32_t mclip;	/* the mirror's clip, backing pixels */
	uint8_t		*vpix[2];
	unsigned long	vframes, vbad;
	EGLint		egl_age;	/* what EGL said this frame */
	struct { struct gbm_bo *bo; unsigned long frame; } seen[8];
	struct gbm_bo	*front;		/* on screen */
	struct gbm_bo	*retired;	/* on screen until the last flip */
	struct gbm_bo	*pending;	/* flipped to, not yet on screen */
};

static struct gles gl;

/* ---- opening, as root ---- */

static bool
has_ext(const char *list, const char *ext)
{
	size_t n = strlen(ext);
	const char *p = list;

	while (p != NULL && (p = strstr(p, ext)) != NULL) {
		if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
			return true;
		p += n;
	}
	return false;
}

/* Software rasterizers lose to pixman: auto doesn't take them. */
static bool
software_renderer(const char *r)
{
	return r == NULL || strstr(r, "llvmpipe") != NULL ||
	    strstr(r, "softpipe") != NULL || strstr(r, "swrast") != NULL;
}

/*
 * The Renderer setting: "pixman" (the default for now), "gles2", or
 * "auto" (gles2 if a GPU renders, else pixman). Opens GBM and EGL on
 * the DRM device; returns false, after saying why, to stay on pixman.
 */
bool
gles_open(struct ws_server *srv, int drm_fd)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	const char *want = srv->cfg.renderer;
	EGLint major, minor;

	if (strcmp(want, "gles2") != 0 && strcmp(want, "auto") != 0)
		return false;
	gl.gbm = gbm_create_device(drm_fd);
	if (gl.gbm == NULL) {
		ws_info("renderer: no GBM on this device; using pixman");
		return false;
	}
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
	    eglGetProcAddress("eglGetPlatformDisplayEXT");
	gl.dpy = get_display != NULL ?
	    get_display(EGL_PLATFORM_GBM_KHR, gl.gbm, NULL) : EGL_NO_DISPLAY;
	if (gl.dpy == EGL_NO_DISPLAY || !eglInitialize(gl.dpy, &major, &minor)) {
		ws_info("renderer: EGL on GBM failed (0x%x); using pixman",
		    eglGetError());
		goto fail;
	}
	ws_info("renderer: EGL %d.%d, %s", major, minor,
	    eglQueryString(gl.dpy, EGL_VENDOR));
	return true;
fail:
	gbm_device_destroy(gl.gbm);
	gl.gbm = NULL;
	gl.dpy = EGL_NO_DISPLAY;
	return false;
}

/* ---- the context and the surface ---- */

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;
	char log[256];

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		ws_err("renderer: shader: %s", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint
program(const char *vs, const char *fs)
{
	GLuint p, v = shader(GL_VERTEX_SHADER, vs), f;
	GLint ok;

	if (v == 0)
		return 0;
	f = shader(GL_FRAGMENT_SHADER, fs);
	if (f == 0) {
		glDeleteShader(v);
		return 0;
	}
	p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "pos");
	glBindAttribLocation(p, 1, "uv");
	glLinkProgram(p);
	glDeleteShader(v);
	glDeleteShader(f);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

/* Positions are backing pixels, top-left origin. */
static const char vs_tex[] =
    "uniform vec2 screen;\n"
    "attribute vec2 pos;\n"
    "attribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "	v_uv = uv;\n"
    "	gl_Position = vec4(pos.x * 2.0 / screen.x - 1.0,\n"
    "	    1.0 - pos.y * 2.0 / screen.y, 0.0, 1.0);\n"
    "}\n";

/* Premultiplied ARGB in, so opacity scales every channel (OVER is
   ONE, ONE_MINUS_SRC_ALPHA). */
static const char fs_tex[] =
    /* texture coordinates need more than mediump's 11 bits on GPUs
       that honour it, past ~2048 texels */
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D tex;\n"
    "uniform float opacity;\n"
    "uniform bool opaque;\n"
    "uniform bool swizzle;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "	vec4 c = texture2D(tex, v_uv);\n"
    "	if (swizzle)\n"
    "		c = c.bgra;\n"
    "	if (opaque)\n"
    "		c.a = 1.0;\n"
    "	gl_FragColor = c * opacity;\n"
    "}\n";

static const char vs_fill[] =
    "uniform vec2 screen;\n"
    "attribute vec2 pos;\n"
    "void main() {\n"
    "	gl_Position = vec4(pos.x * 2.0 / screen.x - 1.0,\n"
    "	    1.0 - pos.y * 2.0 / screen.y, 0.0, 1.0);\n"
    "}\n";

static const char fs_fill[] =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() {\n"
    "	gl_FragColor = color;\n"
    "}\n";

static bool
choose_config(void)
{
	EGLint attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
	EGLConfig cfgs[64];
	EGLint n = 0, id, i;

	if (!eglChooseConfig(gl.dpy, attr, cfgs, 64, &n))
		return false;
	/* the one whose GBM format is the scanout's */
	for (i = 0; i < n; i++)
		if (eglGetConfigAttrib(gl.dpy, cfgs[i], EGL_NATIVE_VISUAL_ID,
		    &id) && (uint32_t)id == GBM_FORMAT_XRGB8888) {
			gl.config = cfgs[i];
			return true;
		}
	return false;
}

static void gles_fini(struct ws_server *srv);

/*
 * GPU buffers need dmabuf import, EGLImage textures and a render node
 * for clients to allocate on: the display's own GPU, as EGL says
 * (EGL_EXT_device_drm_render_node), else the first render node.
 */
static void
dmabuf_setup(const char *glext)
{
	PFNEGLQUERYDISPLAYATTRIBEXTPROC query_display;
	PFNEGLQUERYDEVICESTRINGEXTPROC query_device;
	const char *eglext = eglQueryString(gl.dpy, EGL_EXTENSIONS);
	const char *node = NULL;
	EGLAttrib dev = 0;
	struct dirent *de;
	DIR *d;

	gl.dmabuf = false;
	if (eglext == NULL || glext == NULL ||
	    !has_ext(eglext, "EGL_EXT_image_dma_buf_import") ||
	    !has_ext(glext, "GL_OES_EGL_image"))
		return;
	gl.create_image = (PFNEGLCREATEIMAGEKHRPROC)
	    eglGetProcAddress("eglCreateImageKHR");
	gl.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
	    eglGetProcAddress("eglDestroyImageKHR");
	gl.image_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
	    eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (gl.create_image == NULL || gl.destroy_image == NULL ||
	    gl.image_texture == NULL)
		return;
	query_display = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)
	    eglGetProcAddress("eglQueryDisplayAttribEXT");
	query_device = (PFNEGLQUERYDEVICESTRINGEXTPROC)
	    eglGetProcAddress("eglQueryDeviceStringEXT");
	if (query_display != NULL && query_device != NULL &&
	    query_display(gl.dpy, EGL_DEVICE_EXT, &dev) && dev != 0)
		node = query_device((EGLDeviceEXT)dev,
		    EGL_DRM_RENDER_NODE_FILE_EXT);
	gl.render_node[0] = '\0';
	if (node != NULL)
		strlcpy(gl.render_node, node, sizeof(gl.render_node));
	else if ((d = opendir("/dev/dri")) != NULL) {
		while ((de = readdir(d)) != NULL)
			if (strncmp(de->d_name, "renderD", 7) == 0) {
				snprintf(gl.render_node, sizeof(gl.render_node),
				    "/dev/dri/%s", de->d_name);
				break;
			}
		closedir(d);
	}
	gl.dmabuf = gl.render_node[0] != '\0';
}

/*
 * The GLES2 context, HIGH priority if EGL_IMG_context_priority is there
 * (#74). The level is a hint the driver may lower, so ask EGL what was
 * granted; gl.priority stays NULL without the extension.
 */
static EGLContext
create_context(void)
{
	EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
	    EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG,
	    EGL_NONE };
	EGLContext ctx;
	EGLint level;

	gl.priority = NULL;
	if (!has_ext(eglQueryString(gl.dpy, EGL_EXTENSIONS),
	    "EGL_IMG_context_priority"))
		attr[2] = EGL_NONE;
	ctx = eglCreateContext(gl.dpy, gl.config, EGL_NO_CONTEXT, attr);
	if (ctx == EGL_NO_CONTEXT && attr[2] != EGL_NONE) {
		/* the spec says a hint never fails; don't count on it */
		ws_warn("renderer: high-priority context failed (0x%x); "
		    "trying without", eglGetError());
		attr[2] = EGL_NONE;
		ctx = eglCreateContext(gl.dpy, gl.config, EGL_NO_CONTEXT,
		    attr);
	}
	if (ctx == EGL_NO_CONTEXT || attr[2] == EGL_NONE)
		return ctx;
	if (!eglQueryContext(gl.dpy, ctx, EGL_CONTEXT_PRIORITY_LEVEL_IMG,
	    &level))
		level = 0;
	switch (level) {
	case EGL_CONTEXT_PRIORITY_HIGH_IMG:
		gl.priority = "high";
		break;
	case EGL_CONTEXT_PRIORITY_MEDIUM_IMG:
		gl.priority = "medium";
		break;
	case EGL_CONTEXT_PRIORITY_LOW_IMG:
		gl.priority = "low";
		break;
	default:
		gl.priority = "unknown";
		break;
	}
	return ctx;
}

static int
gles_init(struct ws_server *srv)
{
	const char *ext, *renderer;
	int i;

	/* The backing store as big as the panel only, until T7. */
	if (srv->back_width != srv->panel_width ||
	    srv->back_height != srv->panel_height) {
		ws_info("renderer: gles2 can't scale %ux%u to the %ux%u panel "
		    "yet; using pixman", srv->back_width, srv->back_height,
		    srv->panel_width, srv->panel_height);
		return -1;
	}
	if (gl.ctx != EGL_NO_CONTEXT)
		return 0;	/* same panel, same surface */
	if (gl.dpy == EGL_NO_DISPLAY)
		return -1;
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!choose_config()) {
		ws_info("renderer: no XRGB8888 EGL config; using pixman");
		return -1;
	}
	gl.ctx = create_context();
	gl.width = srv->panel_width;
	gl.height = srv->panel_height;
	gl.surface = gbm_surface_create(gl.gbm, gl.width, gl.height,
	    GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (gl.ctx == EGL_NO_CONTEXT || gl.surface == NULL) {
		ws_info("renderer: no GLES2 context or GBM surface; using "
		    "pixman");
		goto fail;
	}
	gl.egl_surface = eglCreateWindowSurface(gl.dpy, gl.config,
	    (EGLNativeWindowType)gl.surface, NULL);
	if (gl.egl_surface == EGL_NO_SURFACE ||
	    !eglMakeCurrent(gl.dpy, gl.egl_surface, gl.egl_surface, gl.ctx)) {
		ws_info("renderer: EGL surface failed (0x%x); using pixman",
		    eglGetError());
		goto fail;
	}
	renderer = (const char *)glGetString(GL_RENDERER);
	if (strcmp(srv->cfg.renderer, "auto") == 0 &&
	    software_renderer(renderer)) {
		ws_info("renderer: %s is software; using pixman",
		    renderer != NULL ? renderer : "GL");
		goto fail;
	}
	ext = (const char *)glGetString(GL_EXTENSIONS);
	gl.bgra = ext != NULL && has_ext(ext, "GL_EXT_texture_format_BGRA8888");
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl.max_tex);
	gl.verify = getenv("WS_GL_VERIFY") != NULL;
	gl.unpack_row = ext != NULL && has_ext(ext, "GL_EXT_unpack_subimage");
	gl.buffer_age = has_ext(eglQueryString(gl.dpy, EGL_EXTENSIONS),
	    "EGL_EXT_buffer_age");
	dmabuf_setup(ext);
	gl.prog_tex = program(vs_tex, fs_tex);
	gl.prog_fill = program(vs_fill, fs_fill);
	if (gl.prog_tex == 0 || gl.prog_fill == 0)
		goto fail;
	gl.tex_screen = glGetUniformLocation(gl.prog_tex, "screen");
	gl.tex_opacity = glGetUniformLocation(gl.prog_tex, "opacity");
	gl.tex_opaque = glGetUniformLocation(gl.prog_tex, "opaque");
	gl.tex_swizzle = glGetUniformLocation(gl.prog_tex, "swizzle");
	gl.fill_screen = glGetUniformLocation(gl.prog_fill, "screen");
	gl.fill_color = glGetUniformLocation(gl.prog_fill, "color");
	glGenTextures(1, &gl.tex);
	glBindTexture(GL_TEXTURE_2D, gl.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glViewport(0, 0, (GLsizei)gl.width, (GLsizei)gl.height);
	pixman_region32_init(&gl.clip);
	pixman_region32_init(&gl.frame_damage);
	pixman_region32_init(&gl.mclip);
	if (gl.verify) {
		glGenTextures(1, &gl.mirror_tex);
		glBindTexture(GL_TEXTURE_2D, gl.mirror_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)gl.width,
		    (GLsizei)gl.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &gl.mirror_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, gl.mirror_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_TEXTURE_2D, gl.mirror_tex, 0);
		for (i = 0; i < GLES_SIM; i++) {
			glGenTextures(1, &gl.sim_tex[i]);
			glBindTexture(GL_TEXTURE_2D, gl.sim_tex[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
			    (GLsizei)gl.width, (GLsizei)gl.height, 0, GL_RGBA,
			    GL_UNSIGNED_BYTE, NULL);
			glGenFramebuffers(1, &gl.sim_fbo[i]);
			glBindFramebuffer(GL_FRAMEBUFFER, gl.sim_fbo[i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER,
			    GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			    gl.sim_tex[i], 0);
			glClearColor(1, 0, 1, 1);	/* garbage: magenta */
			glClear(GL_COLOR_BUFFER_BIT);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		memset(gl.sim_of, -1, sizeof(gl.sim_of));
		gl.vpix[0] = malloc((size_t)gl.width * gl.height * 4);
		gl.vpix[1] = malloc((size_t)gl.width * gl.height * 4);
		ws_warn("renderer: WS_GL_VERIFY: every frame is checked, slowly");
	}
	for (i = 0; i < GLES_HISTORY; i++)
		pixman_region32_init(&gl.history[i]);
	gl.nhistory = 0;
	ws_info("renderer: gles2 on %s, %ux%u%s%s%s%s%s, GPU priority %s",
	    renderer, gl.width,
	    gl.height, gl.bgra ? ", BGRA textures" : "",
	    gl.unpack_row ? ", row length" : "",
	    gl.buffer_age ? ", buffer age" : "",
	    gl.dmabuf ? ", GPU buffers from " : "",
	    gl.dmabuf ? gl.render_node : "",
	    gl.priority != NULL ? gl.priority :
	    "default (no EGL_IMG_context_priority)");
	return 0;
fail:
	gles_fini(srv);
	return -1;
}

static void gles_window_gone(struct ws_server *srv, struct ws_window *w);

static void
gles_fini(struct ws_server *srv)
{
	struct ws_window *w;
	int i;

	for (w = srv->windows; w != NULL; w = w->next)
		gles_window_gone(srv, w);
	if (gl.prog_tex != 0) {
		pixman_region32_fini(&gl.clip);
		for (i = 0; i < GLES_HISTORY; i++)
			pixman_region32_fini(&gl.history[i]);
	}
	if (gl.dpy != EGL_NO_DISPLAY)
		eglMakeCurrent(gl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		    EGL_NO_CONTEXT);
	if (gl.surface != NULL) {
		if (gl.pending != NULL)
			gbm_surface_release_buffer(gl.surface, gl.pending);
		if (gl.front != NULL)
			gbm_surface_release_buffer(gl.surface, gl.front);
		if (gl.retired != NULL)
			gbm_surface_release_buffer(gl.surface, gl.retired);
		gl.retired = NULL;
	}
	gl.pending = gl.front = NULL;
	if (gl.egl_surface != EGL_NO_SURFACE)
		eglDestroySurface(gl.dpy, gl.egl_surface);
	if (gl.ctx != EGL_NO_CONTEXT)
		eglDestroyContext(gl.dpy, gl.ctx);
	if (gl.surface != NULL)
		gbm_surface_destroy(gl.surface);
	/* never started again in this server's life (compositor.c) */
	if (gl.dpy != EGL_NO_DISPLAY)
		eglTerminate(gl.dpy);
	if (gl.gbm != NULL)
		gbm_device_destroy(gl.gbm);
	gl.dpy = EGL_NO_DISPLAY;
	gl.gbm = NULL;
	gl.egl_surface = EGL_NO_SURFACE;
	gl.ctx = EGL_NO_CONTEXT;
	gl.surface = NULL;
	gl.prog_tex = gl.prog_fill = gl.tex = 0;
}

/* ---- drawing ---- */

/*
 * The GBM buffer this frame draws into last showed the frame `age`
 * frames ago (0: unknown), so it lacks what changed since then: this
 * frame's damage and that of the age - 1 frames before it.
 */
static void
gles_repaint(struct ws_server *srv, pixman_region32_t *region)
{
	EGLint age = 0;
	int i;

	if (gl.buffer_age && !eglQuerySurface(gl.dpy, gl.egl_surface,
	    EGL_BUFFER_AGE_EXT, &age))
		age = 0;
	gl.egl_age = age;
	if (gl.verify) {
		/* the buffer this frame lands in: the one drawn `age` frames
		   ago, or with age 0 one not among the last few */
		unsigned long f = gl.vframes + 1;
		int k = age > 0 && f >= (unsigned long)age ?
		    gl.sim_of[(f - (unsigned long)age) % 64] : -1;

		if (k < 0)
			k = (int)(f % GLES_SIM);
		gl.sim_cur = k;
		gl.sim_of[f % 64] = k;
	}
	pixman_region32_copy(&gl.frame_damage, region);
	/* remember this frame's own damage, newest first */
	pixman_region32_fini(&gl.history[GLES_HISTORY - 1]);
	memmove(&gl.history[1], &gl.history[0],
	    sizeof(gl.history[0]) * (GLES_HISTORY - 1));
	pixman_region32_init(&gl.history[0]);
	pixman_region32_copy(&gl.history[0], region);
	if (age == 0 || age - 1 > gl.nhistory) {
		pixman_region32_union_rect(region, region, 0, 0, srv->width,
		    srv->height);
	} else {
		for (i = 1; i < age; i++)
			pixman_region32_union(region, region, &gl.history[i]);
	}
	if (gl.nhistory < GLES_HISTORY - 1)
		gl.nhistory++;
	if (gl.verify) {
		pixman_box32_t *e1 = pixman_region32_extents(&gl.frame_damage);
		pixman_box32_t *e2 = pixman_region32_extents(region);

		ws_debug("renderer: frame %lu age %d nhist %d own %d,%d-%d,%d "
		    "drawn %d,%d-%d,%d", gl.vframes + 1, age, gl.nhistory,
		    e1->x1, e1->y1, e1->x2, e1->y2, e2->x1, e2->y1, e2->x2,
		    e2->y2);
	}
}

static void
gles_begin(struct ws_server *srv, pixman_region32_t *back)
{
	pixman_box32_t *b;
	int i, n, s = (int)srv->scale;

	pixman_region32_copy(&gl.clip, back);
	glEnable(GL_SCISSOR_TEST);
	if (gl.verify) {
		pixman_region32_clear(&gl.mclip);
		b = pixman_region32_rectangles(&gl.frame_damage, &n);
		for (i = 0; i < n; i++)
			pixman_region32_union_rect(&gl.mclip, &gl.mclip,
			    b[i].x1 * s, b[i].y1 * s,
			    (unsigned)((b[i].x2 - b[i].x1) * s),
			    (unsigned)((b[i].y2 - b[i].y1) * s));
		pixman_region32_intersect(&gl.mclip, &gl.mclip, back);
	}
}

/* Draw once per rectangle of `clip` that meets x, y, w, h. */
static void
clipped_in(pixman_region32_t *clip, int32_t x, int32_t y, int32_t w,
    int32_t h)
{
	pixman_region32_t r;
	pixman_box32_t *b;
	int i, n;

	pixman_region32_init_rect(&r, x, y, (unsigned)w, (unsigned)h);
	pixman_region32_intersect(&r, &r, clip);
	b = pixman_region32_rectangles(&r, &n);
	for (i = 0; i < n; i++) {
		glScissor(b[i].x1, (GLint)gl.height - b[i].y2,
		    b[i].x2 - b[i].x1, b[i].y2 - b[i].y1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	pixman_region32_fini(&r);
}

static void
clipped(int32_t x, int32_t y, int32_t w, int32_t h)
{
	clipped_in(&gl.clip, x, y, w, h);
}

/* Upload an a8r8g8b8 or x8r8g8b8 image (another format is converted). */
static bool
upload(pixman_image_t *img, int *tw, int *th)
{
	pixman_format_code_t f = pixman_image_get_format(img);
	pixman_image_t *conv = NULL;
	int w = pixman_image_get_width(img), h = pixman_image_get_height(img);
	int stride, y;
	uint8_t *data;

	if (w > gl.max_tex || h > gl.max_tex)
		return false;	/* drawn as nothing rather than black */
	if (f != PIXMAN_a8r8g8b8 && f != PIXMAN_x8r8g8b8) {
		conv = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, NULL, 0);
		if (conv == NULL)
			return false;
		pixman_image_composite32(PIXMAN_OP_SRC, img, NULL, conv, 0, 0,
		    0, 0, 0, 0, w, h);
		img = conv;
	}
	data = (uint8_t *)pixman_image_get_data(img);
	stride = pixman_image_get_stride(img);
	if (data == NULL || w <= 0 || h <= 0) {
		if (conv != NULL)
			pixman_image_unref(conv);
		return false;
	}
	/* Native-endian ARGB is BGRA in memory. */
	if (stride == w * 4 || gl.unpack_row) {
		if (stride != w * 4)
			glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / 4);
		glTexImage2D(GL_TEXTURE_2D, 0, gl.bgra ? GL_BGRA_EXT : GL_RGBA,
		    w, h, 0, gl.bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE,
		    data);
		if (stride != w * 4)
			glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, gl.bgra ? GL_BGRA_EXT : GL_RGBA,
		    w, h, 0, gl.bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE,
		    NULL);
		for (y = 0; y < h; y++)
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1,
			    gl.bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE,
			    data + (size_t)y * stride);
	}
	if (conv != NULL)
		pixman_image_unref(conv);
	*tw = w;
	*th = h;
	return true;
}

/* Upload the rectangle x, y, w, h (pixels) of a 32-bit image into the
   bound texture, which is already the image's size. */
static void
upload_rect(pixman_image_t *img, int x, int y, int w, int h)
{
	uint8_t *data = (uint8_t *)pixman_image_get_data(img);
	int stride = pixman_image_get_stride(img), row;
	GLenum f = gl.bgra ? GL_BGRA_EXT : GL_RGBA;

	if (gl.unpack_row) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / 4);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, x);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, y);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, f,
		    GL_UNSIGNED_BYTE, data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
		return;
	}
	for (row = y; row < y + h; row++)
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, row, w, 1, f,
		    GL_UNSIGNED_BYTE, data + (size_t)row * stride +
		    (size_t)x * 4);
}

/* Bring window w's texture up to date with its buffer; bind it. */
static bool
window_texture(struct ws_window *w, pixman_image_t *img, int *tw, int *th)
{
	struct gles_window *gw = w->render;
	int iw = pixman_image_get_width(img), ih = pixman_image_get_height(img);
	int s = w->buffer != NULL && w->buffer->scale ? (int)w->buffer->scale :
	    1;
	pixman_box32_t *b;
	int i, n, x1, y1, x2, y2;

	if (gw == NULL) {
		gw = calloc(1, sizeof(*gw));
		if (gw == NULL)
			return false;
		pixman_region32_init(&gw->dirty);
		gw->all = true;
		w->render = gw;
	}
	if (gw->tex == 0) {
		glGenTextures(1, &gw->tex);
		glBindTexture(GL_TEXTURE_2D, gw->tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		    GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		    GL_CLAMP_TO_EDGE);
		gw->all = true;
	} else
		glBindTexture(GL_TEXTURE_2D, gw->tex);
	/* damage is in points of the buffer it came with: another size,
	   format or scale makes the texture all stale */
	if (gw->tw != iw || gw->th != ih ||
	    gw->format != pixman_image_get_format(img) || gw->scale != s)
		gw->all = true;
	gw->format = pixman_image_get_format(img);
	gw->scale = s;
	if (gw->all) {
		if (!upload(img, &gw->tw, &gw->th))
			return false;
	} else {
		/* only what the client damaged, in buffer pixels */
		b = pixman_region32_rectangles(&gw->dirty, &n);
		for (i = 0; i < n; i++) {
			x1 = b[i].x1 * s < 0 ? 0 : b[i].x1 * s;
			y1 = b[i].y1 * s < 0 ? 0 : b[i].y1 * s;
			x2 = b[i].x2 * s > iw ? iw : b[i].x2 * s;
			y2 = b[i].y2 * s > ih ? ih : b[i].y2 * s;
			if (x2 > x1 && y2 > y1)
				upload_rect(img, x1, y1, x2 - x1, y2 - y1);
		}
	}
	gw->all = false;
	pixman_region32_clear(&gw->dirty);
	*tw = gw->tw;
	*th = gw->th;
	return true;
}

static void
gles_draw(struct ws_server *srv, const struct ws_draw *d)
{
	GLfloat x1 = d->x, y1 = d->y, x2 = d->x + d->w, y2 = d->y + d->h;
	GLfloat u2, v2;
	GLfloat pos[8], uv[8];
	int tw, th;

	(void)srv;
	if (d->w <= 0 || d->h <= 0 || d->opacity <= 0.0)
		return;
	struct ws_buffer *b = d->window != NULL ? d->window->buffer : NULL;
	struct gles_buffer *gb;
	bool opaque, swizzle = !gl.bgra;

	if (b != NULL && b->dmabuf) {
		/* the client's GPU buffer itself: nothing to upload */
		gb = b->render;
		if (gb == NULL)
			return;
		glBindTexture(GL_TEXTURE_2D, gb->tex);
		tw = (int)b->width;
		th = (int)b->height;
		opaque = b->format == WS_FORMAT_XRGB8888;
		swizzle = false;	/* EGL samples it as RGBA */
	} else if (d->window != NULL &&
	    (pixman_image_get_format(d->src) == PIXMAN_a8r8g8b8 ||
	    pixman_image_get_format(d->src) == PIXMAN_x8r8g8b8)) {
		if (!window_texture(d->window, d->src, &tw, &th))
			return;
		opaque = pixman_image_get_format(d->src) == PIXMAN_x8r8g8b8;
	} else {
		glBindTexture(GL_TEXTURE_2D, gl.tex);
		if (!upload(d->src, &tw, &th))
			return;
		opaque = pixman_image_get_format(d->src) == PIXMAN_x8r8g8b8;
	}
	/* the source pixels this covers, as texture coordinates */
	u2 = (GLfloat)(d->w * d->fx / tw);
	v2 = (GLfloat)(d->h * d->fy / th);
	pos[0] = x1; pos[1] = y1; pos[2] = x2; pos[3] = y1;
	pos[4] = x1; pos[5] = y2; pos[6] = x2; pos[7] = y2;
	uv[0] = 0; uv[1] = 0; uv[2] = u2; uv[3] = 0;
	uv[4] = 0; uv[5] = v2; uv[6] = u2; uv[7] = v2;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
	    d->smooth ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
	    d->smooth ? GL_LINEAR : GL_NEAREST);
	glUseProgram(gl.prog_tex);
	glUniform2f(gl.tex_screen, (GLfloat)gl.width, (GLfloat)gl.height);
	glUniform1f(gl.tex_opacity, (GLfloat)(d->opacity < 1.0 ?
	    d->opacity : 1.0));
	/* x8r8g8b8's top byte isn't alpha, as pixman reads it */
	glUniform1i(gl.tex_opaque, opaque);
	glUniform1i(gl.tex_swizzle, swizzle);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	if (d->blend || d->opacity < 1.0)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);
	clipped(d->x, d->y, d->w, d->h);
	if (gl.verify) {
		/* the simulated GBM buffer: the same draw, same clip */
		glBindFramebuffer(GL_FRAMEBUFFER, gl.sim_fbo[gl.sim_cur]);
		clipped(d->x, d->y, d->w, d->h);
		/* the mirror: the image uploaded whole, this frame's damage */
		glBindFramebuffer(GL_FRAMEBUFFER, gl.mirror_fbo);
		glBindTexture(GL_TEXTURE_2D, gl.tex);
		/* a GPU buffer has no CPU pixels: its own texture is the truth */
		if (b != NULL && b->dmabuf && b->render != NULL)
			glBindTexture(GL_TEXTURE_2D,
			    ((struct gles_buffer *)b->render)->tex);
		if ((b != NULL && b->dmabuf) || upload(d->src, &tw, &th)) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			    d->smooth ? GL_LINEAR : GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
			    d->smooth ? GL_LINEAR : GL_NEAREST);
			clipped_in(&gl.mclip, d->x, d->y, d->w, d->h);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}

static void
gles_fill(struct ws_server *srv, const pixman_color_t *color,
    const pixman_box32_t *boxes, int n)
{
	GLfloat pos[8];
	int i;

	(void)srv;
	glUseProgram(gl.prog_fill);
	glUniform2f(gl.fill_screen, (GLfloat)gl.width, (GLfloat)gl.height);
	/* pixman colours are premultiplied, like the blend */
	glUniform4f(gl.fill_color, color->red / 65535.0f,
	    color->green / 65535.0f, color->blue / 65535.0f,
	    color->alpha / 65535.0f);
	glDisableVertexAttribArray(1);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	if (color->alpha == 0xffff)
		glDisable(GL_BLEND);
	else
		glEnable(GL_BLEND);
	for (i = 0; i < n; i++) {
		pos[0] = boxes[i].x1; pos[1] = boxes[i].y1;
		pos[2] = boxes[i].x2; pos[3] = boxes[i].y1;
		pos[4] = boxes[i].x1; pos[5] = boxes[i].y2;
		pos[6] = boxes[i].x2; pos[7] = boxes[i].y2;
		clipped(boxes[i].x1, boxes[i].y1, boxes[i].x2 - boxes[i].x1,
		    boxes[i].y2 - boxes[i].y1);
		if (gl.verify) {
			glBindFramebuffer(GL_FRAMEBUFFER,
			    gl.sim_fbo[gl.sim_cur]);
			clipped(boxes[i].x1, boxes[i].y1, boxes[i].x2 - boxes[i].x1,
			    boxes[i].y2 - boxes[i].y1);
			glBindFramebuffer(GL_FRAMEBUFFER, gl.mirror_fbo);
			clipped_in(&gl.mclip, boxes[i].x1, boxes[i].y1,
			    boxes[i].x2 - boxes[i].x1,
			    boxes[i].y2 - boxes[i].y1);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}
}

/* WS_GL_VERIFY: does the frame about to be shown match the mirror? */
static void
verify_frame(void)
{
	uint32_t *a = (uint32_t *)gl.vpix[0], *m = (uint32_t *)gl.vpix[1];
	uint32_t x, y, bad = 0, x1 = gl.width, y1 = gl.height, x2 = 0, y2 = 0;
	EGLint age = -1;

	if (a == NULL || m == NULL)
		return;
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.sim_fbo[gl.sim_cur]);
	glReadPixels(0, 0, (GLsizei)gl.width, (GLsizei)gl.height, GL_RGBA,
	    GL_UNSIGNED_BYTE, a);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.mirror_fbo);
	glReadPixels(0, 0, (GLsizei)gl.width, (GLsizei)gl.height, GL_RGBA,
	    GL_UNSIGNED_BYTE, m);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	for (y = 0; y < gl.height; y++)
		for (x = 0; x < gl.width; x++) {
			/* the screen has no alpha; the mirror texture does */
			if (((a[y * gl.width + x] ^ m[y * gl.width + x]) &
			    0x00ffffff) == 0)
				continue;
			bad++;
			if (x < x1) x1 = x;
			if (x > x2) x2 = x;
			if (y < y1) y1 = y;
			if (y > y2) y2 = y;
		}
	gl.vframes++;
	if (gl.buffer_age)
		eglQuerySurface(gl.dpy, gl.egl_surface, EGL_BUFFER_AGE_EXT, &age);
	if (bad != 0 && gl.vbad < 2) {
		/* the first two: both images of the area, as PPM */
		char path[64];
		FILE *f;
		int k;
		uint32_t xx, yy, *img;

		for (k = 0; k < 2; k++) {
			img = k == 0 ? a : m;
			snprintf(path, sizeof(path), "/tmp/verify-%lu-%s.ppm",
			    gl.vbad, k == 0 ? "screen" : "mirror");
			if ((f = fopen(path, "wb")) == NULL)
				continue;
			fprintf(f, "P6\n%u %u\n255\n", x2 - x1 + 1, y2 - y1 + 1);
			for (yy = y2 + 1; yy-- > y1;)
				for (xx = x1; xx <= x2; xx++) {
					uint32_t p = img[yy * gl.width + xx];

					fputc((int)(p & 0xff), f);
					fputc((int)((p >> 8) & 0xff), f);
					fputc((int)((p >> 16) & 0xff), f);
				}
			fclose(f);
		}
	}
	if (bad != 0) {
		gl.vbad++;
		/* GL rows are bottom up: say them top down */
		ws_warn("renderer: VERIFY frame %lu: %u pixels differ in "
		    "%u,%u %ux%u (age %d)", gl.vframes, bad, x1,
		    gl.height - 1 - y2, x2 - x1 + 1, y2 - y1 + 1, age);
	} else if (gl.vframes % 300 == 0)
		ws_info("renderer: VERIFY %lu frames, %lu bad", gl.vframes,
		    gl.vbad);
}

static void
gles_end(struct ws_server *srv, pixman_region32_t *back,
    pixman_region32_t *out)
{
	(void)back;
	if (gl.verify)
		verify_frame();

	glDisable(GL_SCISSOR_TEST);
	pixman_region32_clear(&gl.clip);
	if (!eglSwapBuffers(gl.dpy, gl.egl_surface))
		ws_err("renderer: eglSwapBuffers: 0x%x", eglGetError());
	pixman_region32_init_rect(out, 0, 0, srv->panel_width,
	    srv->panel_height);
}

/* ---- scanout (drm.c) ---- */

static void
fb_free(struct gbm_bo *bo, void *data)
{
	struct gbm_device *gbm = gbm_bo_get_device(bo);
	uint32_t fb = (uint32_t)(uintptr_t)data;

	if (fb != 0)
		drmModeRmFB(gbm_device_get_fd(gbm), fb);
}

/* The frame gles_end swapped, as a DRM framebuffer to flip to. */
static bool
gles_scanout(struct ws_server *srv, uint32_t *fb_id)
{
	uint32_t handles[4] = { 0 }, strides[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { 0 }, mod;
	struct gbm_bo *bo;
	uint32_t fb = 0;
	int fd, i, planes, r = -1;

	(void)srv;
	/* a frame that never reached the screen (a failed flip and
	   SetCrtc): give its buffer back, or GBM runs out */
	if (gl.pending != NULL) {
		gbm_surface_release_buffer(gl.surface, gl.pending);
		gl.pending = NULL;
	}
	bo = gbm_surface_lock_front_buffer(gl.surface);
	if (bo == NULL) {
		ws_err("renderer: no front buffer to show");
		return false;
	}
	fb = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
	if (fb == 0) {
		fd = gbm_device_get_fd(gl.gbm);
		mod = gbm_bo_get_modifier(bo);
		planes = gbm_bo_get_plane_count(bo);
		for (i = 0; i < planes && i < 4; i++) {
			handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
			strides[i] = gbm_bo_get_stride_for_plane(bo, i);
			offsets[i] = gbm_bo_get_offset(bo, i);
			mods[i] = mod;
		}
		if (mod != DRM_FORMAT_MOD_INVALID)
			r = drmModeAddFB2WithModifiers(fd, gl.width, gl.height,
			    gbm_bo_get_format(bo), handles, strides, offsets,
			    mods, &fb, DRM_MODE_FB_MODIFIERS);
		if (r != 0)
			r = drmModeAddFB2(fd, gl.width, gl.height,
			    gbm_bo_get_format(bo), handles, strides, offsets,
			    &fb, 0);
		if (r != 0) {
			ws_err("renderer: drmModeAddFB2: %s", strerror(-r));
			gbm_surface_release_buffer(gl.surface, bo);
			return false;
		}
		gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb, fb_free);
	}
	if (gl.verify) {
		/* the age this buffer really had, against what EGL said */
		int j, k = -1, ns = (int)(sizeof(gl.seen) / sizeof(gl.seen[0]));
		long real = 0;

		for (j = 0; j < ns; j++)
			if (gl.seen[j].bo == bo)
				k = j;
		if (k >= 0)
			real = (long)(gl.vframes - gl.seen[k].frame);
		else
			for (k = 0; k < ns && gl.seen[k].bo != NULL; k++)
				;
		if (k < ns) {
			gl.seen[k].bo = bo;
			gl.seen[k].frame = gl.vframes;
		}
		if (real != gl.egl_age)
			ws_warn("renderer: VERIFY frame %lu: buffer %p was %ld "
			    "frames old, EGL said %d", gl.vframes, (void *)bo,
			    real, gl.egl_age);
	}
	gl.pending = bo;
	*fb_id = fb;
	return true;
}

/* The flip to the last scanout buffer is done: the one before is free. */
static void
gles_presented(struct ws_server *srv)
{
	(void)srv;
	if (gl.pending == NULL)
		return;
	/*
	 * The buffer the flip left may still be scanned out for a moment
	 * after the flip's event (the vc4 HVS latches later): drawing
	 * into it then shows half a frame. Keep it one more flip.
	 */
	if (gl.retired != NULL)
		gbm_surface_release_buffer(gl.surface, gl.retired);
	gl.retired = gl.front;
	gl.front = gl.pending;
	gl.pending = NULL;
}

/* ---- windows ---- */

static void
gles_window_commit(struct ws_server *srv, struct ws_window *w,
    pixman_region32_t *damage)
{
	struct gles_window *gw = w->render;

	(void)srv;
	if (gw == NULL)
		return;		/* no texture yet: the first draw makes it */
	if (damage == NULL || w->buffer == NULL)
		gw->all = true;	/* a detach: whatever comes next is new */
	else
		pixman_region32_union(&gw->dirty, &gw->dirty, damage);
}

static void
gles_window_gone(struct ws_server *srv, struct ws_window *w)
{
	struct gles_window *gw = w->render;

	(void)srv;
	if (gw == NULL)
		return;
	if (gw->tex != 0)
		glDeleteTextures(1, &gw->tex);
	pixman_region32_fini(&gw->dirty);
	free(gw);
	w->render = NULL;
}

/* ---- GPU buffers ---- */

static uint32_t
drm_format(uint32_t ws_format)
{
	return ws_format == WS_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888 :
	    DRM_FORMAT_ARGB8888;
}

/* The render node, and the modifiers EGL imports for both formats, the
   GPU's own (tiled) ones before LINEAR. */
static int
gles_dmabuf_formats(struct ws_server *srv, ws_reply_t *r)
{
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query;
	static const uint32_t fmts[] = { WS_FORMAT_ARGB8888,
	    WS_FORMAT_XRGB8888 };
	EGLuint64KHR mods[32];
	EGLBoolean ext_only[32];
	EGLint n;
	int f, i, pass;
	uint32_t k = 0;

	(void)srv;
	if (!gl.dmabuf)
		return ENOTSUP;
	strlcpy(r->u.dmabuf.render_node, gl.render_node,
	    sizeof(r->u.dmabuf.render_node));
	query = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)
	    eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	for (pass = 0; pass < 2; pass++)	/* tiled, then LINEAR */
		for (f = 0; f < 2; f++) {
			n = 0;
			if (query == NULL || !query(gl.dpy, drm_format(fmts[f]),
			    32, mods, ext_only, &n))
				n = 0;
			for (i = 0; i < n; i++) {
				if (ext_only[i] ||
				    (mods[i] == DRM_FORMAT_MOD_LINEAR) != pass)
					continue;
				if (k == WS_MAX_DMABUF_FORMATS)
					goto done;
				r->u.dmabuf.formats[k].format = fmts[f];
				r->u.dmabuf.formats[k].modifier = mods[i];
				k++;
			}
			if (n == 0 && pass == 1 && k < WS_MAX_DMABUF_FORMATS) {
				r->u.dmabuf.formats[k].format = fmts[f];
				r->u.dmabuf.formats[k].modifier =
				    DRM_FORMAT_MOD_LINEAR;
				k++;
			}
		}
done:
	r->u.dmabuf.count = k;
	return 0;
}

static bool
gles_buffer_import(struct ws_server *srv, struct ws_buffer *b)
{
	struct gles_buffer *gb;
	EGLint attr[] = {
		EGL_WIDTH, (EGLint)b->width,
		EGL_HEIGHT, (EGLint)b->height,
		EGL_LINUX_DRM_FOURCC_EXT, (EGLint)drm_format(b->format),
		EGL_DMA_BUF_PLANE0_FD_EXT, b->fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)b->offset,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)b->stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
		    (EGLint)(b->modifier & 0xffffffff),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(b->modifier >> 32),
		EGL_NONE
	};

	(void)srv;
	if (!gl.dmabuf)
		return false;
	if (b->modifier == DRM_FORMAT_MOD_INVALID)
		attr[12] = EGL_NONE;	/* implicit: no modifier attributes */
	gb = calloc(1, sizeof(*gb));
	if (gb == NULL)
		return false;
	gb->image = gl.create_image(gl.dpy, EGL_NO_CONTEXT,
	    EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	if (gb->image == EGL_NO_IMAGE_KHR) {
		ws_debug("renderer: dmabuf import failed (0x%x)",
		    eglGetError());
		free(gb);
		return false;
	}
	glGenTextures(1, &gb->tex);
	glBindTexture(GL_TEXTURE_2D, gb->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.image_texture(GL_TEXTURE_2D, (GLeglImageOES)gb->image);
	if (glGetError() != GL_NO_ERROR) {
		glDeleteTextures(1, &gb->tex);
		gl.destroy_image(gl.dpy, gb->image);
		free(gb);
		return false;
	}
	b->render = gb;
	return true;
}

static void
gles_buffer_gone(struct ws_server *srv, struct ws_buffer *b)
{
	struct gles_buffer *gb = b->render;

	(void)srv;
	if (gb == NULL)
		return;
	glDeleteTextures(1, &gb->tex);
	gl.destroy_image(gl.dpy, gb->image);
	free(gb);
	b->render = NULL;
}

/*
 * A GPU buffer's pixels for the CPU, as premultiplied a8r8g8b8: drawn
 * into a framebuffer of its size (flipped, so the rows read back top
 * first; swizzled, so RGBA reads as native ARGB), then read. Leaves the
 * frame being drawn as it was.
 */
static pixman_image_t *
gles_buffer_read(struct ws_server *srv, struct ws_buffer *b)
{
	struct gles_buffer *gb = b->render;
	pixman_image_t *img;
	GLuint tex, fbo;
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLfloat w = (GLfloat)b->width, h = (GLfloat)b->height;
	GLfloat pos[8] = { 0, 0, w, 0, 0, h, w, h };
	GLfloat uv[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };

	(void)srv;
	if (gb == NULL)
		return NULL;
	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)b->width,
	    (int)b->height, NULL, (int)b->width * 4);
	if (img == NULL)
		return NULL;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)b->width,
	    (GLsizei)b->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, tex, 0);
	glViewport(0, 0, (GLsizei)b->width, (GLsizei)b->height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glUseProgram(gl.prog_tex);
	glUniform2f(gl.tex_screen, w, h);
	glUniform1f(gl.tex_opacity, 1.0f);
	glUniform1i(gl.tex_opaque, b->format == WS_FORMAT_XRGB8888);
	glUniform1i(gl.tex_swizzle, 1);
	glBindTexture(GL_TEXTURE_2D, gb->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glReadPixels(0, 0, (GLsizei)b->width, (GLsizei)b->height, GL_RGBA,
	    GL_UNSIGNED_BYTE, pixman_image_get_data(img));
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	glViewport(0, 0, (GLsizei)gl.width, (GLsizei)gl.height);
	if (scissor)
		glEnable(GL_SCISSOR_TEST);
	return img;
}

const struct ws_renderer_ops gles_renderer_ops = {
	.name = "gles2",
	.init = gles_init,
	.fini = gles_fini,
	.repaint = gles_repaint,
	.begin = gles_begin,
	.draw = gles_draw,
	.fill = gles_fill,
	.end = gles_end,
	.scanout = gles_scanout,
	.presented = gles_presented,
	.window_commit = gles_window_commit,
	.window_gone = gles_window_gone,
	.dmabuf_formats = gles_dmabuf_formats,
	.buffer_import = gles_buffer_import,
	.buffer_gone = gles_buffer_gone,
	.buffer_read = gles_buffer_read,
};
