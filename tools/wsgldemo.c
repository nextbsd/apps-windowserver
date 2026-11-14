/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wsgldemo: a client that draws with GLES into GPU buffers (dmabufs)
 * and hands them to WindowServer without a copy (WS_SERVER_DMABUF).
 * It animates a full window every frame, paced by WS_EVENT_FRAME_DONE,
 * and reports the frame rate and its own CPU time; the server's is in
 * top(1). With -d it is a desktop-level window covering the screen, as
 * InnerSpace's is. With -f it fills with a changing colour instead,
 * which costs the GPU next to nothing: what is left is the server's.
 *
 *   wsgldemo [-df] [-s SECONDS] [WIDTHxHEIGHT]  (points; default 800x600)
 */

#include <sys/resource.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <WindowServer/WSClient.h>
#include <WindowServer/WSProtocol.h>

#define NBUF	3

struct buf {
	struct gbm_bo	*bo;
	EGLImageKHR	image;
	GLuint		tex, fbo;
	ws_buffer_t	*ws;
	int		busy;		/* with the server until released */
};

static struct buf bufs[NBUF];
static int frame_pending;

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static double
cpu(void)
{
	struct rusage r;

	getrusage(RUSAGE_SELF, &r);
	return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 +
	    r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}

static void
handler(ws_connection_t *c, const ws_event_t *ev, void *ctx)
{
	int i;

	(void)c;
	(void)ctx;
	if (ev->hdr.type == WS_EVENT_FRAME_DONE)
		frame_pending = 0;
	else if (ev->hdr.type == WS_EVENT_BUFFER_RELEASE)
		for (i = 0; i < NBUF; i++)
			if (bufs[i].ws != NULL &&
			    ws_buffer_id(bufs[i].ws) == ev->u.buffer.buffer_id)
				bufs[i].busy = 0;
}

static GLuint
program(void)
{
	static const char *vs =
	    "attribute vec2 p; varying vec2 uv;\n"
	    "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
	/* a kaleidoscope: six mirrored sectors of moving colour */
	static const char *fs =
	    "precision mediump float; varying vec2 uv; uniform float t;\n"
	    "uniform vec2 aspect;\n"
	    "void main() {\n"
	    "	vec2 q = (uv - 0.5) * aspect;\n"
	    "	float a = atan(q.y, q.x), r = length(q);\n"
	    "	a = abs(mod(a + t * 0.2, 1.0472) - 0.5236);\n"
	    "	vec2 k = vec2(cos(a), sin(a)) * r * 6.0;\n"
	    "	vec3 c = 0.5 + 0.5 * cos(t + k.xyx + vec3(0.0, 2.0, 4.0));\n"
	    "	gl_FragColor = vec4(c * smoothstep(1.2, 0.0, r), 1.0);\n"
	    "}\n";
	const char *src[2] = { vs, fs };
	GLenum ty[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram(), s;
	GLint ok;
	char log[256];
	int i;

	for (i = 0; i < 2; i++) {
		s = glCreateShader(ty[i]);
		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "wsgldemo: shader: %s\n", log);
			exit(1);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	return p;
}

int
main(int argc, char **argv)
{
	static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;
	ws_dmabuf_formats_t fmts;
	const ws_screen_info_t *scr;
	ws_connection_t *c;
	ws_rect_t frame = { 0, 0, 800, 600 };
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLContext ctx;
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	struct pollfd pfd;
	uint64_t mod;
	uint32_t win, scale, pw, ph, fmt;
	double secs = 10, t0, c0, last, worst = 0;
	int desktop = 0, flat = 0, ch, fd, i, frames = 0, b;
	GLint ut;
	GLuint prog;

	while ((ch = getopt(argc, argv, "dfs:")) != -1) {
		switch (ch) {
		case 'd':
			desktop = 1;
			break;
		case 'f':
			flat = 1;
			break;
		case 's':
			secs = atof(optarg);
			break;
		default:
			fprintf(stderr, "usage: wsgldemo [-df] [-s SECONDS] "
			    "[WIDTHxHEIGHT]\n");
			return 2;
		}
	}
	if (optind < argc && sscanf(argv[optind], "%dx%d", &frame.width,
	    &frame.height) != 2) {
		fprintf(stderr, "wsgldemo: bad size %s\n", argv[optind]);
		return 2;
	}

	c = ws_connect("wsgldemo", 0);
	if (c == NULL) {
		perror("wsgldemo: ws_connect");
		return 1;
	}
	if (ws_dmabuf_formats(c, &fmts) < 0 || fmts.count == 0) {
		fprintf(stderr, "wsgldemo: the server takes no GPU buffers "
		    "(Renderer gles2?): %s\n", strerror(errno));
		return 1;
	}
	scr = ws_main_screen(c);
	scale = scr != NULL && scr->scale ? scr->scale : 1;
	if (desktop && scr != NULL) {
		frame.width = (int32_t)scr->width;
		frame.height = (int32_t)scr->height;
	}
	pw = (uint32_t)frame.width * scale;
	ph = (uint32_t)frame.height * scale;
	mod = fmts.formats[0].modifier;
	fmt = fmts.formats[0].format;
	printf("render node %s, %ux%u pixels, format %u modifier 0x%llx\n",
	    fmts.render_node, pw, ph, fmt, (unsigned long long)mod);

	fd = open(fmts.render_node, O_RDWR | O_CLOEXEC);
	gbm = fd >= 0 ? gbm_create_device(fd) : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
	    eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = gbm != NULL && get_display != NULL ?
	    get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "wsgldemo: no EGL on %s\n", fmts.render_node);
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
	    ctx_attr);
	if (ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "wsgldemo: no GLES2 context\n");
		return 1;
	}
	create_image = (PFNEGLCREATEIMAGEKHRPROC)
	    eglGetProcAddress("eglCreateImageKHR");
	image_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
	    eglGetProcAddress("glEGLImageTargetTexture2DOES");
	printf("GL_RENDERER %s\n", glGetString(GL_RENDERER));

	for (i = 0; i < NBUF; i++) {
		struct buf *bf = &bufs[i];
		int dfd;

		bf->bo = gbm_bo_create_with_modifiers2(gbm, pw, ph,
		    fmt == WS_FORMAT_XRGB8888 ? GBM_FORMAT_XRGB8888 :
		    GBM_FORMAT_ARGB8888, &mod, 1, GBM_BO_USE_RENDERING);
		if (bf->bo == NULL) {
			fprintf(stderr, "wsgldemo: can't allocate a buffer\n");
			return 1;
		}
		dfd = gbm_bo_get_fd(bf->bo);
		{
			EGLint attr[] = {
				EGL_WIDTH, (EGLint)pw, EGL_HEIGHT, (EGLint)ph,
				EGL_LINUX_DRM_FOURCC_EXT,
				    (EGLint)gbm_bo_get_format(bf->bo),
				EGL_DMA_BUF_PLANE0_FD_EXT, dfd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT,
				    (EGLint)gbm_bo_get_offset(bf->bo, 0),
				EGL_DMA_BUF_PLANE0_PITCH_EXT,
				    (EGLint)gbm_bo_get_stride(bf->bo),
				EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
				    (EGLint)(mod & 0xffffffff),
				EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
				    (EGLint)(mod >> 32),
				EGL_NONE
			};
			bf->image = create_image(dpy, EGL_NO_CONTEXT,
			    EGL_LINUX_DMA_BUF_EXT, NULL, attr);
		}
		glGenTextures(1, &bf->tex);
		glBindTexture(GL_TEXTURE_2D, bf->tex);
		image_texture(GL_TEXTURE_2D, (GLeglImageOES)bf->image);
		glGenFramebuffers(1, &bf->fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, bf->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_TEXTURE_2D, bf->tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
		    GL_FRAMEBUFFER_COMPLETE) {
			fprintf(stderr, "wsgldemo: can't draw into it\n");
			return 1;
		}
		bf->ws = ws_buffer_create_dmabuf(c, pw, ph, scale, fmt, dfd,
		    gbm_bo_get_offset(bf->bo, 0), gbm_bo_get_stride(bf->bo),
		    mod);
		close(dfd);
		if (bf->ws == NULL) {
			perror("wsgldemo: ws_buffer_create_dmabuf");
			return 1;
		}
	}

	win = ws_window_create(c, &frame, desktop ? WS_LEVEL_DESKTOP :
	    WS_LEVEL_NORMAL, fmt == WS_FORMAT_XRGB8888 ? WS_WINDOW_OPAQUE : 0,
	    "wsgldemo");
	if (win == 0) {
		perror("wsgldemo: ws_window_create");
		return 1;
	}
	prog = program();
	glUseProgram(prog);
	ut = glGetUniformLocation(prog, "t");
	glUniform2f(glGetUniformLocation(prog, "aspect"),
	    (GLfloat)pw / (GLfloat)ph, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, (GLsizei)pw, (GLsizei)ph);

	pfd.fd = ws_connection_fd(c);
	pfd.events = POLLIN;
	t0 = last = now();
	c0 = cpu();
	while (now() - t0 < secs) {
		/* wait for the last frame to show, and a free buffer */
		for (;;) {
			if (ws_dispatch(c, handler, NULL) < 0)
				return 1;
			for (b = 0; b < NBUF && bufs[b].busy; b++)
				;
			if (!frame_pending && b < NBUF)
				break;
			(void)poll(&pfd, 1, 100);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, bufs[b].fbo);
		if (flat) {
			glClearColor((frames % 60) / 60.0f, 0.3f, 0.5f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		} else {
			glUniform1f(ut, (GLfloat)(now() - t0));
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glFlush();	/* implicit sync fences it in the dmabuf */
		ws_window_attach(c, win, bufs[b].ws);
		ws_window_commit(c, win, WS_COMMIT_FRAME_DONE);
		bufs[b].busy = 1;
		frame_pending = 1;
		if (frames == 0)
			ws_window_order(c, win, WS_ORDER_ABOVE, 0);
		frames++;
		if (frames > 3 && now() - last > worst)
			worst = now() - last;
		last = now();
	}
	printf("%d frames in %.1fs: %.1f fps, worst %.1f ms, wsgldemo CPU "
	    "%.0f%% of a core\n", frames, now() - t0, frames / (now() - t0),
	    worst * 1000, 100 * (cpu() - c0) / (now() - t0));
	ws_window_destroy(c, win);
	ws_disconnect(c);
	return 0;
}
