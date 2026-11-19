/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The GPU renderer (E38): GLES2 through EGL on GBM, on the DRM output's
 * own device. On a Raspberry Pi 5 that's vc4, and Mesa's kmsro renders
 * on v3d; on other machines whatever Mesa (or a vendor's GBM) drives.
 *
 * Each frame is drawn into the desktop: a texture (framebuffer object)
 * of the whole backing store, which keeps everything drawn before, so a
 * frame draws only its own damage. Then each display (display.c) gets
 * its part of it copied, scaled where the sizes differ, into a GBM
 * surface of its panel's size, only what that surface lacks (EGL buffer
 * age); the DRM output flips each display to its new buffer
 * (gles_scanout), and the buffer before goes back to GBM once the flip
 * is done (gles_presented). That copy also scales a "looks like" size to
 * the panel, shows a 1x display beside a 2x one, and letterboxes a
 * mirror of another shape. srv->out stays NULL: the GBM buffers can't be
 * read back here (Mesa can't map a scanout buffer). Captures of the
 * screen read the main display's part with gles_screen_read instead
 * (T8); snapshots still need the pixman renderer.
 *
 * gles_open() runs as root, before privileges are dropped, because
 * eglInitialize is where Mesa opens the render node.
 *
 * Each window has its own texture, and a commit uploads only what it
 * damaged (gles_window_commit marks it, the next draw uploads it). Other
 * images (tiles, the cursor, drag images, overview previews) are still
 * uploaded whole each time they're drawn (T6).
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
 * Screen captures (T8): while something reads the screen, the main
 * display's part of each frame is also copied into a texture of its
 * panel's size, the same way as to the display. gles_screen_read reads
 * back where it changed since the last read, into a CPU image. Nobody
 * reading for a few seconds stops it. On GLES 3 the read goes into a
 * pixel pack buffer with a fence and lands at a later read, so it never
 * waits for the GPU: the image is a frame behind, and the reader is told
 * where. The first read after the copy starts still waits, so it gets a
 * whole screen.
 *
 * WS_GL_VERIFY draws every frame a second time into a mirror of the
 * desktop, the slow sure way (only the frame's own damage, every image
 * uploaded whole), and compares the two.
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
/* GLES 3.0, for screen capture readback; gl2.h has none of it */
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER		0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ			0x88E1
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT			0x0001
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE	0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT	0x00000001
#define GL_ALREADY_SIGNALED		0x911A
#define GL_TIMEOUT_EXPIRED		0x911B
#define GL_CONDITION_SATISFIED		0x911C
#define GL_WAIT_FAILED			0x911D
#endif
typedef struct __GLsync *gles_sync;
typedef gles_sync (GL_APIENTRYP gles_fence_sync)(GLenum, GLbitfield);
typedef GLenum (GL_APIENTRYP gles_client_wait_sync)(gles_sync, GLbitfield,
    uint64_t);
typedef void (GL_APIENTRYP gles_delete_sync)(gles_sync);
typedef void *(GL_APIENTRYP gles_map_buffer_range)(GLenum, GLintptr,
    GLsizeiptr, GLbitfield);
typedef GLboolean (GL_APIENTRYP gles_unmap_buffer)(GLenum);

/* Damage of the frames before this one, newest first (buffer age). */
#define GLES_HISTORY	4

/* Screen capture readbacks in flight at once (pixel pack buffers). */
#define GLES_CAP_PBOS	2

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

/* A display's GBM surface and its buffers (d->render). */
struct gles_display {
	struct gbm_surface *surface;
	EGLSurface	egl_surface;
	uint32_t	width, height;	/* the panel */
	/* the backing pixels of the frames before this one, newest
	   first (buffer age) */
	pixman_region32_t history[GLES_HISTORY];
	int		nhistory;
	bool		swapped;	/* a new frame for gles_scanout */
	struct gbm_bo	*front;		/* on screen */
	struct gbm_bo	*retired;	/* on screen until the last flip */
	struct gbm_bo	*pending;	/* flipped to, not yet on screen */
};

struct gles {
	struct gbm_device *gbm;
	EGLDisplay	dpy;
	EGLConfig	config;
	EGLContext	ctx;
	EGLSurface	current;	/* made current with ctx */
	/* the desktop: the whole backing store */
	GLuint		desk_fbo, desk_tex;
	uint32_t	width, height;	/* its size */
	bool		desk_fresh;	/* nothing in it yet: draw it all */
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
	GLuint		prog_tex, prog_fill;
	GLint		tex_screen, tex_opacity, tex_opaque, tex_swizzle;
	GLint		fill_screen, fill_color;
	GLuint		tex;		/* reused for every image (T3) */
	pixman_region32_t clip;		/* begin..end, backing pixels */
	/* WS_GL_VERIFY: a second desktop, drawn the slow sure way, and
	   compared with the first every frame */
	bool		verify;
	GLuint		mirror_fbo, mirror_tex;
	pixman_region32_t frame_damage;	/* points: this frame's own */
	pixman_region32_t mclip;	/* the mirror's clip, backing pixels */
	uint8_t		*vpix[2];
	unsigned long	vframes, vbad;
	/* screen captures (T8): the main display's copy, and what of it the
	   CPU image lacks */
	bool		cap_on;
	bool		cap_fresh;	/* not copied whole yet */
	uint64_t	cap_used;	/* ws_now_usec() of the last read */
	GLuint		cap_fbo, cap_tex;
	uint32_t	cap_w, cap_h;	/* the main display's panel */
	pixman_region32_t cap_dirty;	/* not read back, panel pixels */
	pixman_image_t	*cap_img;	/* x8r8g8b8, the panel's size */
	uint8_t		*cap_rows;	/* glReadPixels lands here */
	bool		read_bgra;	/* GL_EXT_read_format_bgra */
	/* readback without waiting (GLES 3): each read goes into a pixel
	   pack buffer behind a fence, and is copied out once it signals */
	bool		pbo;
	bool		cap_sync;	/* the next read waits: the first */
	uint64_t	cap_serial;	/* bumped when cap_img changes */
	int		cap_next;	/* the buffer to use next, the oldest */
	struct {
		GLuint		buf;
		gles_sync	fence;		/* NULL: not in flight */
		pixman_region32_t rects;	/* what it holds, panel pixels */
	} cap_pb[GLES_CAP_PBOS];
	gles_fence_sync	fence_sync;
	gles_client_wait_sync client_wait_sync;
	gles_delete_sync delete_sync;
	gles_map_buffer_range map_buffer_range;
	gles_unmap_buffer unmap_buffer;
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
 * The Renderer setting: "auto" (the default: gles2 if a GPU renders,
 * else pixman), "gles2" (even on a software renderer) or "pixman". Opens GBM and EGL on
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

/*
 * Readback without waiting needs GLES 3 (pixel pack buffers, fences,
 * glMapBufferRange). A context asked for as GLES 2 is 3.x on Mesa when
 * the GPU can. WS_GL_SYNC_READ turns it off, to compare.
 */
static bool
pbo_setup(void)
{
	const char *v = (const char *)glGetString(GL_VERSION);

	if (v == NULL || strncmp(v, "OpenGL ES ", 10) != 0 ||
	    atoi(v + 10) < 3 || getenv("WS_GL_SYNC_READ") != NULL)
		return false;
	gl.fence_sync = (gles_fence_sync)eglGetProcAddress("glFenceSync");
	gl.client_wait_sync = (gles_client_wait_sync)
	    eglGetProcAddress("glClientWaitSync");
	gl.delete_sync = (gles_delete_sync)eglGetProcAddress("glDeleteSync");
	gl.map_buffer_range = (gles_map_buffer_range)
	    eglGetProcAddress("glMapBufferRange");
	gl.unmap_buffer = (gles_unmap_buffer)
	    eglGetProcAddress("glUnmapBuffer");
	return gl.fence_sync != NULL && gl.client_wait_sync != NULL &&
	    gl.delete_sync != NULL && gl.map_buffer_range != NULL &&
	    gl.unmap_buffer != NULL;
}

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
static void cap_stop(void);

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

/* ---- displays' surfaces ---- */

static bool
make_current(EGLSurface surf)
{
	if (gl.current == surf)
		return true;
	if (!eglMakeCurrent(gl.dpy, surf, surf, gl.ctx)) {
		ws_err("renderer: eglMakeCurrent: 0x%x", eglGetError());
		return false;
	}
	gl.current = surf;
	return true;
}

static void
surface_free(struct ws_display *d)
{
	struct gles_display *gd = d->render;
	int i;

	if (gd == NULL)
		return;
	if (gl.current == gd->egl_surface && gd->egl_surface != EGL_NO_SURFACE) {
		eglMakeCurrent(gl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		    EGL_NO_CONTEXT);
		gl.current = EGL_NO_SURFACE;
	}
	if (gd->surface != NULL) {
		if (gd->pending != NULL)
			gbm_surface_release_buffer(gd->surface, gd->pending);
		if (gd->front != NULL)
			gbm_surface_release_buffer(gd->surface, gd->front);
		if (gd->retired != NULL)
			gbm_surface_release_buffer(gd->surface, gd->retired);
	}
	if (gd->egl_surface != EGL_NO_SURFACE)
		eglDestroySurface(gl.dpy, gd->egl_surface);
	if (gd->surface != NULL)
		gbm_surface_destroy(gd->surface);
	for (i = 0; i < GLES_HISTORY; i++)
		pixman_region32_fini(&gd->history[i]);
	free(gd);
	d->render = NULL;
}

/* d's GBM surface at its panel's size (the one it has, if it fits). */
static bool
surface_make(struct ws_display *d)
{
	struct gles_display *gd = d->render;
	int i;

	if (gd != NULL && gd->width == d->panel_width &&
	    gd->height == d->panel_height)
		return true;
	surface_free(d);
	gd = calloc(1, sizeof(*gd));
	if (gd == NULL)
		return false;
	for (i = 0; i < GLES_HISTORY; i++)
		pixman_region32_init(&gd->history[i]);
	gd->egl_surface = EGL_NO_SURFACE;
	d->render = gd;
	gd->width = d->panel_width;
	gd->height = d->panel_height;
	gd->surface = gbm_surface_create(gl.gbm, gd->width, gd->height,
	    GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (gd->surface != NULL)
		gd->egl_surface = eglCreateWindowSurface(gl.dpy, gl.config,
		    (EGLNativeWindowType)gd->surface, NULL);
	if (gd->egl_surface == EGL_NO_SURFACE) {
		ws_info("renderer: no GBM surface for display %s (0x%x)",
		    d->connector, eglGetError());
		surface_free(d);
		return false;
	}
	return true;
}

static void
gles_display_gone(struct ws_server *srv, struct ws_display *d)
{
	(void)srv;
	surface_free(d);
}

/* The desktop texture, width x height (the backing store). */
static bool
desk_make(uint32_t w, uint32_t h)
{
	if (gl.desk_fbo != 0 && gl.width == w && gl.height == h)
		return true;
	if (w > (uint32_t)gl.max_tex || h > (uint32_t)gl.max_tex) {
		ws_info("renderer: gles2 can't hold a %ux%u desktop (largest "
		    "texture %d); using pixman", w, h, gl.max_tex);
		return false;
	}
	if (gl.desk_fbo != 0)
		glDeleteFramebuffers(1, &gl.desk_fbo);
	if (gl.desk_tex != 0)
		glDeleteTextures(1, &gl.desk_tex);
	glGenTextures(1, &gl.desk_tex);
	glBindTexture(GL_TEXTURE_2D, gl.desk_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
	    GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &gl.desk_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.desk_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, gl.desk_tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		ws_info("renderer: no %ux%u desktop framebuffer; using pixman",
		    w, h);
		return false;
	}
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.width = w;
	gl.height = h;
	gl.desk_fresh = true;
	if (gl.verify) {
		if (gl.mirror_fbo != 0)
			glDeleteFramebuffers(1, &gl.mirror_fbo);
		if (gl.mirror_tex != 0)
			glDeleteTextures(1, &gl.mirror_tex);
		glGenTextures(1, &gl.mirror_tex);
		glBindTexture(GL_TEXTURE_2D, gl.mirror_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w,
		    (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &gl.mirror_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, gl.mirror_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_TEXTURE_2D, gl.mirror_tex, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		free(gl.vpix[0]);
		free(gl.vpix[1]);
		gl.vpix[0] = malloc((size_t)w * h * 4);
		gl.vpix[1] = malloc((size_t)w * h * 4);
	}
	return true;
}

/* The context, once: made current with the first display's surface. */
static int
context_init(struct ws_server *srv, struct ws_display *first)
{
	const char *ext, *renderer;
	int i;

	eglBindAPI(EGL_OPENGL_ES_API);
	if (!choose_config()) {
		ws_info("renderer: no XRGB8888 EGL config; using pixman");
		return -1;
	}
	gl.ctx = create_context();
	if (gl.ctx == EGL_NO_CONTEXT || !surface_make(first)) {
		ws_info("renderer: no GLES2 context or GBM surface; using "
		    "pixman");
		return -1;
	}
	if (!make_current(((struct gles_display *)first->render)->egl_surface))
		return -1;
	renderer = (const char *)glGetString(GL_RENDERER);
	if (strcmp(srv->cfg.renderer, "auto") == 0 &&
	    software_renderer(renderer)) {
		ws_info("renderer: %s is software; using pixman",
		    renderer != NULL ? renderer : "GL");
		return -1;
	}
	ext = (const char *)glGetString(GL_EXTENSIONS);
	gl.bgra = ext != NULL && has_ext(ext, "GL_EXT_texture_format_BGRA8888");
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl.max_tex);
	gl.verify = getenv("WS_GL_VERIFY") != NULL;
	gl.read_bgra = ext != NULL && has_ext(ext, "GL_EXT_read_format_bgra");
	gl.unpack_row = ext != NULL && has_ext(ext, "GL_EXT_unpack_subimage");
	gl.buffer_age = has_ext(eglQueryString(gl.dpy, EGL_EXTENSIONS),
	    "EGL_EXT_buffer_age");
	dmabuf_setup(ext);
	gl.pbo = pbo_setup();
	gl.prog_tex = program(vs_tex, fs_tex);
	gl.prog_fill = program(vs_fill, fs_fill);
	if (gl.prog_tex == 0 || gl.prog_fill == 0)
		return -1;
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
	pixman_region32_init(&gl.clip);
	pixman_region32_init(&gl.frame_damage);
	pixman_region32_init(&gl.mclip);
	pixman_region32_init(&gl.cap_dirty);
	for (i = 0; i < GLES_CAP_PBOS; i++)
		pixman_region32_init(&gl.cap_pb[i].rects);
	gl.cap_on = false;
	if (gl.verify)
		ws_warn("renderer: WS_GL_VERIFY: every frame is checked, slowly");
	ws_info("renderer: gles2 on %s%s%s%s%s%s, largest texture %d, GPU "
	    "priority %s", renderer, gl.bgra ? ", BGRA textures" : "",
	    gl.unpack_row ? ", row length" : "",
	    gl.buffer_age ? ", buffer age" : "",
	    gl.dmabuf ? ", GPU buffers from " : "",
	    gl.dmabuf ? gl.render_node : "", gl.max_tex,
	    gl.priority != NULL ? gl.priority :
	    "default (no EGL_IMG_context_priority)");
	return 0;
}

/*
 * The desktop and every display's surface for the current layout
 * (display.c): made the first time, and again for what changed.
 */
static int
gles_init(struct ws_server *srv)
{
	struct ws_display *d, *first = NULL;
	unsigned i;

	if (gl.dpy == EGL_NO_DISPLAY)
		return -1;
	for (i = 0; i < srv->ndisplays && first == NULL; i++)
		if (srv->displays[i]->on)
			first = srv->displays[i];
	if (first == NULL)
		return -1;
	if (gl.ctx == EGL_NO_CONTEXT && context_init(srv, first) < 0)
		goto fail;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!d->on) {
			surface_free(d);
			continue;
		}
		if (!surface_make(d))
			goto fail;
		/* the surface may be new: it has nothing */
		((struct gles_display *)d->render)->nhistory = 0;
	}
	if (!make_current(((struct gles_display *)first->render)->egl_surface) ||
	    !desk_make(srv->back_width, srv->back_height))
		goto fail;
	if (gl.cap_on && srv->main != NULL &&
	    (gl.cap_w != srv->main->panel_width ||
	    gl.cap_h != srv->main->panel_height))
		cap_stop();	/* started again at the main display's size */
	ws_info("renderer: gles2 desktop %ux%u, %u display%s", gl.width,
	    gl.height, srv->ndisplays, srv->ndisplays == 1 ? "" : "s");
	return 0;
fail:
	gles_fini(srv);
	return -1;
}

static void gles_window_gone(struct ws_server *srv, struct ws_window *w);

/* ---- screen captures (T8) ---- */

/* A capture reads nothing for this long: stop the second copy. */
#define GLES_CAP_IDLE_USEC	3000000
/* More rectangles than this to read back: read their extents instead,
   one glReadPixels */
#define GLES_CAP_RECTS		32
/* The longest the first read waits for the GPU (ns) */
#define GLES_CAP_WAIT_NSEC	1000000000ull

static void
cap_stop(void)
{
	int i;

	for (i = 0; i < GLES_CAP_PBOS; i++) {
		if (gl.cap_pb[i].fence != NULL)
			gl.delete_sync(gl.cap_pb[i].fence);
		gl.cap_pb[i].fence = NULL;
		if (gl.cap_pb[i].buf != 0)
			glDeleteBuffers(1, &gl.cap_pb[i].buf);
		gl.cap_pb[i].buf = 0;
		pixman_region32_clear(&gl.cap_pb[i].rects);
	}
	if (gl.cap_fbo != 0)
		glDeleteFramebuffers(1, &gl.cap_fbo);
	if (gl.cap_tex != 0)
		glDeleteTextures(1, &gl.cap_tex);
	gl.cap_fbo = gl.cap_tex = 0;
	if (gl.cap_img != NULL)
		pixman_image_unref(gl.cap_img);
	gl.cap_img = NULL;
	free(gl.cap_rows);
	gl.cap_rows = NULL;
	if (gl.cap_on)
		ws_info("renderer: screen capture copy stopped");
	gl.cap_on = false;
}

static bool
cap_start(struct ws_server *srv)
{
	int i;

	if (srv->main == NULL)
		return false;
	gl.cap_w = srv->main->panel_width;
	gl.cap_h = srv->main->panel_height;
	gl.cap_img = pixman_image_create_bits(PIXMAN_x8r8g8b8,
	    (int)gl.cap_w, (int)gl.cap_h, NULL, 0);
	gl.cap_rows = malloc((size_t)gl.cap_w * gl.cap_h * 4);
	if (gl.cap_img == NULL || gl.cap_rows == NULL) {
		cap_stop();
		return false;
	}
	glGenTextures(1, &gl.cap_tex);
	glBindTexture(GL_TEXTURE_2D, gl.cap_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)gl.cap_w,
	    (GLsizei)gl.cap_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &gl.cap_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.cap_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, gl.cap_tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		ws_warn("renderer: no framebuffer for screen captures");
		cap_stop();
		return false;
	}
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (gl.pbo) {
		/* each big enough for the whole screen: the rectangles of
		   one read don't overlap */
		while (glGetError() != GL_NO_ERROR)
			;
		for (i = 0; i < GLES_CAP_PBOS; i++) {
			glGenBuffers(1, &gl.cap_pb[i].buf);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, gl.cap_pb[i].buf);
			glBufferData(GL_PIXEL_PACK_BUFFER,
			    (GLsizeiptr)gl.cap_w * gl.cap_h * 4, NULL,
			    GL_STREAM_READ);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (glGetError() != GL_NO_ERROR) {
			ws_warn("renderer: no pixel buffers for screen "
			    "captures; reading back the slow way");
			for (i = 0; i < GLES_CAP_PBOS; i++) {
				glDeleteBuffers(1, &gl.cap_pb[i].buf);
				gl.cap_pb[i].buf = 0;
			}
			gl.pbo = false;
		}
	}
	gl.cap_next = 0;
	pixman_region32_clear(&gl.cap_dirty);
	gl.cap_on = true;
	gl.cap_fresh = true;
	gl.cap_sync = true;
	ws_info("renderer: screen capture copy started (%ux%u, %s readback%s)",
	    gl.cap_w, gl.cap_h, gl.read_bgra ? "BGRA" : "RGBA",
	    gl.pbo ? ", pixel buffers" : ", blocking");
	return true;
}

/* The rectangles to read back of r: r's own, or its extents when there
   are too many (r becomes them). */
static pixman_box32_t *
cap_boxes(pixman_region32_t *r, int *n)
{
	pixman_box32_t *b = pixman_region32_rectangles(r, n);

	if (*n > GLES_CAP_RECTS) {
		pixman_region32_reset(r, pixman_region32_extents(r));
		b = pixman_region32_rectangles(r, n);
	}
	return b;
}

/* Box b, read back as GL rows (bottom up, w * 4 bytes each), into the
   CPU image. */
static void
cap_copy(const pixman_box32_t *b, const uint8_t *rows)
{
	uint32_t *img = pixman_image_get_data(gl.cap_img);
	int stride = pixman_image_get_stride(gl.cap_img) / 4;
	int x, y, w = b->x2 - b->x1, h = b->y2 - b->y1;
	const uint8_t *src;

	gl.cap_serial++;
	for (y = 0; y < h; y++) {
		uint32_t *dst = img + (size_t)(b->y2 - 1 - y) * stride + b->x1;

		src = rows + (size_t)y * w * 4;
		if (gl.read_bgra) {
			memcpy(dst, src, (size_t)w * 4);
			continue;
		}
		for (x = 0; x < w; x++)	/* RGBA bytes to ARGB */
			dst[x] = (uint32_t)src[x * 4] << 16 |
			    (uint32_t)src[x * 4 + 1] << 8 | src[x * 4 + 2];
	}
}

/* Read r back now, waiting for the GPU: the slow way. */
static void
cap_read_now(pixman_region32_t *r)
{
	pixman_box32_t *b;
	int i, n, w, h;

	glBindFramebuffer(GL_FRAMEBUFFER, gl.cap_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	b = cap_boxes(r, &n);
	for (i = 0; i < n; i++) {
		w = b[i].x2 - b[i].x1;
		h = b[i].y2 - b[i].y1;
		if (w <= 0 || h <= 0)
			continue;
		/* GL rows are bottom up */
		glReadPixels(b[i].x1, (GLint)gl.cap_h - b[i].y2, w, h,
		    gl.read_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE,
		    gl.cap_rows);
		cap_copy(&b[i], gl.cap_rows);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	pixman_region32_clear(r);
}

/* Start reading cap_dirty back into pixel buffer k, rectangle after
   rectangle; a fence says when it's there. */
static void
cap_read_start(int k)
{
	pixman_box32_t *b;
	uintptr_t off = 0;
	int i, n, w, h;

	pixman_region32_copy(&gl.cap_pb[k].rects, &gl.cap_dirty);
	pixman_region32_clear(&gl.cap_dirty);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.cap_fbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, gl.cap_pb[k].buf);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	b = cap_boxes(&gl.cap_pb[k].rects, &n);
	for (i = 0; i < n; i++) {
		w = b[i].x2 - b[i].x1;
		h = b[i].y2 - b[i].y1;
		if (w <= 0 || h <= 0)
			continue;
		/* with a pack buffer bound, the pointer is an offset */
		glReadPixels(b[i].x1, (GLint)gl.cap_h - b[i].y2, w, h,
		    gl.read_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE,
		    (void *)off);
		off += (uintptr_t)w * h * 4;
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.cap_pb[k].fence = gl.fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();	/* so the fence can signal without us */
	gl.cap_next = (k + 1) % GLES_CAP_PBOS;
}

/*
 * Pixel buffer k into the CPU image once its fence signalled; false
 * while it's still in flight. wait: wait for it, and if it doesn't come
 * (or the buffer fails), what it held is read again instead.
 */
static bool
cap_read_end(int k, bool wait)
{
	pixman_box32_t *b;
	const uint8_t *p = NULL;
	uintptr_t off = 0;
	GLenum r;
	int i, n;

	r = gl.client_wait_sync(gl.cap_pb[k].fence,
	    wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
	    wait ? GLES_CAP_WAIT_NSEC : 0);
	if (r == GL_TIMEOUT_EXPIRED && !wait)
		return false;
	gl.delete_sync(gl.cap_pb[k].fence);
	gl.cap_pb[k].fence = NULL;
	glBindBuffer(GL_PIXEL_PACK_BUFFER, gl.cap_pb[k].buf);
	if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED)
		p = gl.map_buffer_range(GL_PIXEL_PACK_BUFFER, 0,
		    (GLsizeiptr)gl.cap_w * gl.cap_h * 4, GL_MAP_READ_BIT);
	if (p != NULL) {
		/* in the order cap_read_start read them */
		b = pixman_region32_rectangles(&gl.cap_pb[k].rects, &n);
		for (i = 0; i < n; i++) {
			if (b[i].x2 <= b[i].x1 || b[i].y2 <= b[i].y1)
				continue;
			cap_copy(&b[i], p + off);
			off += (uintptr_t)(b[i].x2 - b[i].x1) *
			    (b[i].y2 - b[i].y1) * 4;
		}
		gl.unmap_buffer(GL_PIXEL_PACK_BUFFER);
	} else {
		pixman_region32_union(&gl.cap_dirty, &gl.cap_dirty,
		    &gl.cap_pb[k].rects);
		if (r != GL_TIMEOUT_EXPIRED) {
			ws_warn("renderer: screen capture pixel buffer failed "
			    "(0x%x); reading back the slow way", glGetError());
			gl.pbo = false;
		}
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	pixman_region32_clear(&gl.cap_pb[k].rects);
	return true;
}

/*
 * The main display for captures, in its panel pixels. Reads that landed are
 * copied in; what changed since starts reading back, and *behind (if
 * not NULL) says where the image is older than the screen. wait (and
 * the first read of a copy) reads it all now instead.
 */
static pixman_image_t *
gles_screen_read(struct ws_server *srv, bool wait, pixman_region32_t *behind,
    uint64_t *serial)
{
	int i, k;

	if (behind != NULL)
		pixman_region32_clear(behind);
	*serial = gl.cap_serial;
	if (gl.ctx == EGL_NO_CONTEXT)
		return NULL;
	gl.cap_used = ws_now_usec();
	if (!gl.cap_on) {
		if (!cap_start(srv))
			return NULL;
		damage_all(srv);	/* the next frame copies it whole */
	}
	if (gl.cap_fresh) {
		/* black until then */
		if (behind != NULL)
			pixman_region32_union_rect(behind, behind, 0, 0,
			    gl.cap_w, gl.cap_h);
		*serial = 0;
		return gl.cap_img;
	}
	wait = wait || gl.cap_sync || !gl.pbo;
	/* the reads in flight, oldest first, up to one not done yet */
	for (i = 0; i < GLES_CAP_PBOS; i++) {
		k = (gl.cap_next + i) % GLES_CAP_PBOS;
		if (gl.cap_pb[k].fence == NULL)
			continue;
		if (!gl.pbo) {
			/* the buffers failed: read it again the slow way */
			gl.delete_sync(gl.cap_pb[k].fence);
			gl.cap_pb[k].fence = NULL;
			pixman_region32_union(&gl.cap_dirty, &gl.cap_dirty,
			    &gl.cap_pb[k].rects);
			pixman_region32_clear(&gl.cap_pb[k].rects);
		} else if (!cap_read_end(k, wait))
			break;
	}
	if (pixman_region32_not_empty(&gl.cap_dirty)) {
		if (wait || !gl.pbo)
			cap_read_now(&gl.cap_dirty);
		else if (gl.cap_pb[gl.cap_next].fence == NULL)
			cap_read_start(gl.cap_next);
		/* else every buffer is in flight: the next read */
	}
	gl.cap_sync = false;
	if (behind != NULL) {
		pixman_region32_copy(behind, &gl.cap_dirty);
		for (k = 0; k < GLES_CAP_PBOS; k++)
			if (gl.cap_pb[k].fence != NULL)
				pixman_region32_union(behind, behind,
				    &gl.cap_pb[k].rects);
	}
	*serial = gl.cap_serial;
	return gl.cap_img;
}

static void
gles_fini(struct ws_server *srv)
{
	struct ws_window *w;
	unsigned i;

	for (w = srv->windows; w != NULL; w = w->next)
		gles_window_gone(srv, w);
	if (gl.prog_tex != 0) {
		cap_stop();
		pixman_region32_fini(&gl.cap_dirty);
		for (i = 0; i < GLES_CAP_PBOS; i++)
			pixman_region32_fini(&gl.cap_pb[i].rects);
		pixman_region32_fini(&gl.clip);
		pixman_region32_fini(&gl.frame_damage);
		pixman_region32_fini(&gl.mclip);
	}
	if (gl.ctx != EGL_NO_CONTEXT && gl.current != EGL_NO_SURFACE) {
		if (gl.desk_fbo != 0)
			glDeleteFramebuffers(1, &gl.desk_fbo);
		if (gl.desk_tex != 0)
			glDeleteTextures(1, &gl.desk_tex);
		if (gl.mirror_fbo != 0)
			glDeleteFramebuffers(1, &gl.mirror_fbo);
		if (gl.mirror_tex != 0)
			glDeleteTextures(1, &gl.mirror_tex);
	}
	gl.desk_fbo = gl.desk_tex = gl.mirror_fbo = gl.mirror_tex = 0;
	free(gl.vpix[0]);
	free(gl.vpix[1]);
	gl.vpix[0] = gl.vpix[1] = NULL;
	for (i = 0; i < srv->ndisplays; i++)
		surface_free(srv->displays[i]);
	if (gl.dpy != EGL_NO_DISPLAY)
		eglMakeCurrent(gl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		    EGL_NO_CONTEXT);
	gl.current = EGL_NO_SURFACE;
	if (gl.ctx != EGL_NO_CONTEXT)
		eglDestroyContext(gl.dpy, gl.ctx);
	/* never started again in this server's life (compositor.c) */
	if (gl.dpy != EGL_NO_DISPLAY)
		eglTerminate(gl.dpy);
	if (gl.gbm != NULL)
		gbm_device_destroy(gl.gbm);
	gl.dpy = EGL_NO_DISPLAY;
	gl.gbm = NULL;
	gl.ctx = EGL_NO_CONTEXT;
	gl.prog_tex = gl.prog_fill = gl.tex = 0;
	gl.width = gl.height = 0;
}

/* ---- drawing ---- */

/*
 * The desktop keeps everything drawn before, so a frame draws only its
 * own damage; a new desktop texture is drawn whole once. The displays'
 * buffers are brought up to date from it in gles_end.
 */
static void
gles_repaint(struct ws_server *srv, pixman_region32_t *region)
{
	if (gl.desk_fresh) {
		pixman_region32_union_rect(region, region, 0, 0, srv->width,
		    srv->height);
		gl.desk_fresh = false;
	}
	pixman_region32_copy(&gl.frame_damage, region);
}

static void
gles_begin(struct ws_server *srv, pixman_region32_t *back)
{
	int s = (int)srv->scale;
	pixman_box32_t *b;
	int i, n;

	pixman_region32_copy(&gl.clip, back);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.desk_fbo);
	glViewport(0, 0, (GLsizei)gl.width, (GLsizei)gl.height);
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

/* Draw once per rectangle of `clip` that meets x, y, w, h, in a target
   `height` pixels high. */
static void
clipped_in(pixman_region32_t *clip, int32_t x, int32_t y, int32_t w,
    int32_t h, uint32_t height)
{
	pixman_region32_t r;
	pixman_box32_t *b;
	int i, n;

	pixman_region32_init_rect(&r, x, y, (unsigned)w, (unsigned)h);
	pixman_region32_intersect(&r, &r, clip);
	b = pixman_region32_rectangles(&r, &n);
	for (i = 0; i < n; i++) {
		glScissor(b[i].x1, (GLint)height - b[i].y2,
		    b[i].x2 - b[i].x1, b[i].y2 - b[i].y1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	pixman_region32_fini(&r);
}

static void
clipped(int32_t x, int32_t y, int32_t w, int32_t h)
{
	clipped_in(&gl.clip, x, y, w, h, gl.height);
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
			clipped_in(&gl.mclip, d->x, d->y, d->w, d->h,
			    gl.height);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, gl.desk_fbo);
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
			glBindFramebuffer(GL_FRAMEBUFFER, gl.mirror_fbo);
			clipped_in(&gl.mclip, boxes[i].x1, boxes[i].y1,
			    boxes[i].x2 - boxes[i].x1,
			    boxes[i].y2 - boxes[i].y1, gl.height);
			glBindFramebuffer(GL_FRAMEBUFFER, gl.desk_fbo);
		}
	}
}

/* WS_GL_VERIFY: does the desktop match the mirror? */
static void
verify_frame(void)
{
	uint32_t *a = (uint32_t *)gl.vpix[0], *m = (uint32_t *)gl.vpix[1];
	uint32_t x, y, bad = 0, x1 = gl.width, y1 = gl.height, x2 = 0, y2 = 0;

	if (a == NULL || m == NULL)
		return;
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.desk_fbo);
	glReadPixels(0, 0, (GLsizei)gl.width, (GLsizei)gl.height, GL_RGBA,
	    GL_UNSIGNED_BYTE, a);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.mirror_fbo);
	glReadPixels(0, 0, (GLsizei)gl.width, (GLsizei)gl.height, GL_RGBA,
	    GL_UNSIGNED_BYTE, m);
	for (y = 0; y < gl.height; y++)
		for (x = 0; x < gl.width; x++) {
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
	if (bad != 0) {
		gl.vbad++;
		/* GL rows are bottom up: say them top down */
		ws_warn("renderer: VERIFY frame %lu: %u pixels differ in "
		    "%u,%u %ux%u", gl.vframes, bad, x1, gl.height - 1 - y2,
		    x2 - x1 + 1, y2 - y1 + 1);
	} else if (gl.vframes % 300 == 0)
		ws_info("renderer: VERIFY %lu frames, %lu bad", gl.vframes,
		    gl.vbad);
}

/*
 * Copy `region` (panel pixels of a target w x h, the bound framebuffer)
 * from the desktop: d's backing pixels into its dest rectangle.
 */
static void
copy_out(struct ws_display *d, pixman_region32_t *region, uint32_t w,
    uint32_t h)
{
	GLfloat pos[8], uv[8];
	GLfloat x1 = d->dest.x, y1 = d->dest.y;
	GLfloat x2 = d->dest.x + d->dest.width;
	GLfloat y2 = d->dest.y + d->dest.height;
	GLfloat u1 = (GLfloat)d->back.x / gl.width;
	GLfloat u2 = (GLfloat)(d->back.x + d->back.width) / gl.width;
	/* the desktop's rows are bottom up */
	GLfloat v1 = 1.0f - (GLfloat)d->back.y / gl.height;
	GLfloat v2 = 1.0f - (GLfloat)(d->back.y + d->back.height) / gl.height;
	bool same = d->back.width == d->dest.width &&
	    d->back.height == d->dest.height;

	pos[0] = x1; pos[1] = y1; pos[2] = x2; pos[3] = y1;
	pos[4] = x1; pos[5] = y2; pos[6] = x2; pos[7] = y2;
	uv[0] = u1; uv[1] = v1; uv[2] = u2; uv[3] = v1;
	uv[4] = u1; uv[5] = v2; uv[6] = u2; uv[7] = v2;
	glViewport(0, 0, (GLsizei)w, (GLsizei)h);
	glBindTexture(GL_TEXTURE_2D, gl.desk_tex);
	/* scaled down 2:1, linear filtering at the pixel centres averages
	   each 2 x 2 block */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
	    same ? GL_NEAREST : GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
	    same ? GL_NEAREST : GL_LINEAR);
	glUseProgram(gl.prog_tex);
	glUniform2f(gl.tex_screen, (GLfloat)w, (GLfloat)h);
	glUniform1f(gl.tex_opacity, 1.0f);
	glUniform1i(gl.tex_opaque, 1);
	glUniform1i(gl.tex_swizzle, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDisable(GL_BLEND);
	glEnable(GL_SCISSOR_TEST);
	clipped_in(region, d->dest.x, d->dest.y, d->dest.width,
	    d->dest.height, h);
}

/*
 * Bring display d's next buffer up to date: what changed this frame on
 * it, and what the buffer missed before (its age), copied from the
 * desktop. Nothing changed on it: no new buffer.
 */
static void
display_frame(struct ws_display *d, pixman_region32_t *out)
{
	struct gles_display *gd = d->render;
	pixman_region32_t own, region;
	EGLint age = 0;
	int i;

	gd->swapped = false;
	display_back_to_panel(d, out, &own);
	if (!pixman_region32_not_empty(&own)) {
		pixman_region32_fini(&own);
		return;
	}
	if (!make_current(gd->egl_surface)) {
		pixman_region32_fini(&own);
		return;
	}
	if (gl.buffer_age && !eglQuerySurface(gl.dpy, gd->egl_surface,
	    EGL_BUFFER_AGE_EXT, &age))
		age = 0;
	pixman_region32_init(&region);
	pixman_region32_copy(&region, &own);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (age == 0 || age - 1 > gd->nhistory) {
		/* a buffer of unknown age: all of it, black around a
		   letterboxed mirror */
		pixman_region32_union_rect(&region, &region, 0, 0,
		    gd->width, gd->height);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	} else {
		for (i = 1; i < age; i++)
			pixman_region32_union(&region, &region,
			    &gd->history[i - 1]);
	}
	copy_out(d, &region, gd->width, gd->height);
	/* remember this frame's own, newest first */
	pixman_region32_fini(&gd->history[GLES_HISTORY - 1]);
	memmove(&gd->history[1], &gd->history[0],
	    sizeof(gd->history[0]) * (GLES_HISTORY - 1));
	pixman_region32_init(&gd->history[0]);
	pixman_region32_copy(&gd->history[0], &own);
	if (gd->nhistory < GLES_HISTORY - 1)
		gd->nhistory++;
	pixman_region32_fini(&region);
	pixman_region32_fini(&own);
	glDisable(GL_SCISSOR_TEST);
	if (!eglSwapBuffers(gl.dpy, gd->egl_surface))
		ws_err("renderer: eglSwapBuffers (%s): 0x%x", d->connector,
		    eglGetError());
	else
		gd->swapped = true;
}

static void
gles_end(struct ws_server *srv, pixman_region32_t *back,
    pixman_region32_t *out)
{
	pixman_region32_t cap;
	struct ws_display *d;
	unsigned i;

	if (gl.verify)
		verify_frame();
	glDisable(GL_SCISSOR_TEST);
	pixman_region32_clear(&gl.clip);
	pixman_region32_init(out);
	pixman_region32_copy(out, back);
	if (gl.cap_on && srv->main != NULL && srv->main->render != NULL) {
		/* the capture copy: the main display's part, as it shows */
		display_back_to_panel(srv->main, out, &cap);
		if (gl.cap_fresh) {
			pixman_region32_union_rect(&cap, &cap, 0, 0,
			    gl.cap_w, gl.cap_h);
			gl.cap_fresh = false;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, gl.cap_fbo);
		copy_out(srv->main, &cap, gl.cap_w, gl.cap_h);
		pixman_region32_union(&gl.cap_dirty, &gl.cap_dirty, &cap);
		pixman_region32_fini(&cap);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (ws_now_usec() - gl.cap_used > GLES_CAP_IDLE_USEC)
			cap_stop();
	}
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d->on && d->render != NULL)
			display_frame(d, out);
	}
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

/* The frame gles_end swapped on d, as a DRM framebuffer to flip to;
   false when d has no new frame. */
static bool
gles_scanout(struct ws_server *srv, struct ws_display *d, uint32_t *fb_id)
{
	struct gles_display *gd = d->render;
	uint32_t handles[4] = { 0 }, strides[4] = { 0 }, offsets[4] = { 0 };
	uint64_t mods[4] = { 0 }, mod;
	struct gbm_bo *bo;
	uint32_t fb = 0;
	int fd, i, planes, r = -1;

	(void)srv;
	if (gd == NULL || !gd->swapped)
		return false;
	gd->swapped = false;
	/* a frame that never reached the screen (a failed flip and
	   SetCrtc): give its buffer back, or GBM runs out */
	if (gd->pending != NULL) {
		gbm_surface_release_buffer(gd->surface, gd->pending);
		gd->pending = NULL;
	}
	bo = gbm_surface_lock_front_buffer(gd->surface);
	if (bo == NULL) {
		ws_err("renderer: no front buffer to show on %s",
		    d->connector);
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
			r = drmModeAddFB2WithModifiers(fd, gd->width,
			    gd->height, gbm_bo_get_format(bo), handles,
			    strides, offsets, mods, &fb,
			    DRM_MODE_FB_MODIFIERS);
		if (r != 0)
			r = drmModeAddFB2(fd, gd->width, gd->height,
			    gbm_bo_get_format(bo), handles, strides, offsets,
			    &fb, 0);
		if (r != 0) {
			ws_err("renderer: drmModeAddFB2: %s", strerror(-r));
			gbm_surface_release_buffer(gd->surface, bo);
			return false;
		}
		gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb, fb_free);
	}
	gd->pending = bo;
	*fb_id = fb;
	return true;
}

/* The flip to d's last scanout buffer is done: the one before is free. */
static void
gles_presented(struct ws_server *srv, struct ws_display *d)
{
	struct gles_display *gd = d->render;

	(void)srv;
	if (gd == NULL || gd->pending == NULL)
		return;
	/*
	 * The buffer the flip left may still be scanned out for a moment
	 * after the flip's event (the vc4 HVS latches later): drawing
	 * into it then shows half a frame. Keep it one more flip.
	 */
	if (gd->retired != NULL)
		gbm_surface_release_buffer(gd->surface, gd->retired);
	gd->retired = gd->front;
	gd->front = gd->pending;
	gd->pending = NULL;
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
	.display_gone = gles_display_gone,
	.window_commit = gles_window_commit,
	.window_gone = gles_window_gone,
	.dmabuf_formats = gles_dmabuf_formats,
	.buffer_import = gles_buffer_import,
	.buffer_gone = gles_buffer_gone,
	.buffer_read = gles_buffer_read,
	.screen_read = gles_screen_read,
};
