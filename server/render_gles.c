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
 * Still to come (nextbsd/nextbsd#642): uploading only what changed and
 * one texture per window (T4); repainting only the damage, by buffer
 * age (T5) -- until then every frame repaints, and uploads, everything;
 * a backing store of another size than the panel (T7).
 */

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
#endif

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
	GLuint		prog_tex, prog_fill;
	GLint		tex_screen, tex_opacity, tex_opaque, tex_swizzle;
	GLint		fill_screen, fill_color;
	GLuint		tex;		/* reused for every image (T3) */
	pixman_region32_t clip;		/* begin..end, backing pixels */
	struct gbm_bo	*front;		/* on screen */
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

static int
gles_init(struct ws_server *srv)
{
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *ext, *renderer;

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
	gl.ctx = eglCreateContext(gl.dpy, gl.config, EGL_NO_CONTEXT, ctx_attr);
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
	gl.unpack_row = ext != NULL && has_ext(ext, "GL_EXT_unpack_subimage");
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
	ws_info("renderer: gles2 on %s, %ux%u%s%s", renderer, gl.width,
	    gl.height, gl.bgra ? ", BGRA textures" : "",
	    gl.unpack_row ? ", row length" : "");
	return 0;
fail:
	gles_fini(srv);
	return -1;
}

static void
gles_fini(struct ws_server *srv)
{
	(void)srv;
	if (gl.dpy != EGL_NO_DISPLAY)
		eglMakeCurrent(gl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		    EGL_NO_CONTEXT);
	if (gl.surface != NULL) {
		if (gl.pending != NULL)
			gbm_surface_release_buffer(gl.surface, gl.pending);
		if (gl.front != NULL)
			gbm_surface_release_buffer(gl.surface, gl.front);
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

/* Until buffer age (T5), a GBM buffer holds an old frame: draw it all. */
static void
gles_repaint(struct ws_server *srv, pixman_region32_t *region)
{
	pixman_region32_union_rect(region, region, 0, 0, srv->width,
	    srv->height);
}

static void
gles_begin(struct ws_server *srv, pixman_region32_t *back)
{
	(void)srv;
	pixman_region32_copy(&gl.clip, back);
	glEnable(GL_SCISSOR_TEST);
}

/* Run draw() once per clip rectangle that meets x, y, w, h. */
static void
clipped(int32_t x, int32_t y, int32_t w, int32_t h)
{
	pixman_region32_t r;
	pixman_box32_t *b;
	int i, n;

	pixman_region32_init_rect(&r, x, y, (unsigned)w, (unsigned)h);
	pixman_region32_intersect(&r, &r, &gl.clip);
	b = pixman_region32_rectangles(&r, &n);
	for (i = 0; i < n; i++) {
		glScissor(b[i].x1, (GLint)gl.height - b[i].y2,
		    b[i].x2 - b[i].x1, b[i].y2 - b[i].y1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	pixman_region32_fini(&r);
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
	glBindTexture(GL_TEXTURE_2D, gl.tex);
	if (!upload(d->src, &tw, &th))
		return;
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
	glUniform1i(gl.tex_opaque,
	    pixman_image_get_format(d->src) == PIXMAN_x8r8g8b8);
	glUniform1i(gl.tex_swizzle, !gl.bgra);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	if (d->blend || d->opacity < 1.0)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);
	clipped(d->x, d->y, d->w, d->h);
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
	}
}

static void
gles_end(struct ws_server *srv, pixman_region32_t *back,
    pixman_region32_t *out)
{
	(void)back;
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
	if (gl.front != NULL)
		gbm_surface_release_buffer(gl.surface, gl.front);
	gl.front = gl.pending;
	gl.pending = NULL;
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
};
