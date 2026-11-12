/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Virtual terminal handling for DRM mode, following the same steps as
 * the NextBSD XLibre console patch (bsd_init.c: KD_GRAPHICS, K_RAW with
 * a raw termios, VT_SETMODE VT_PROCESS), written from scratch here.
 *
 * WindowServer takes a free VT, switches to it, and from then on
 * vt(4) asks before switching away: SIGUSR2 arrives, the server drops
 * DRM master and input, and acknowledges with VT_RELDISP. On the way
 * back the same signal arrives and the server takes master again and
 * repaints. Everything is restored on exit, including after a crash.
 */

#include <sys/types.h>
#include <sys/consio.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/kbio.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "server.h"

#define VT_SIGNAL	SIGUSR2

/* Saved console state, also used by the crash handler. */
static struct {
	int		fd;
	int		kbmode;
	struct termios	tios;
	bool		have_tios;
	bool		have_kbmode;
	int		orig_vt;
	bool		active;		/* we own the VT */
} vt_saved = { .fd = -1 };

static void
vt_restore(void)
{
	struct vt_mode vm;
	int fd = vt_saved.fd;

	if (fd < 0)
		return;
	ioctl(fd, KDSETMODE, KD_TEXT);
	if (vt_saved.have_kbmode)
		ioctl(fd, KDSKBMODE, vt_saved.kbmode);
	if (vt_saved.have_tios)
		tcsetattr(fd, TCSANOW, &vt_saved.tios);
	memset(&vm, 0, sizeof(vm));
	vm.mode = VT_AUTO;
	ioctl(fd, VT_SETMODE, &vm);
}

static void
crash_handler(int sig)
{
	/* Only async-signal-safe calls (ioctl, tcsetattr) in vt_restore. */
	vt_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

void
vt_install_crash_handlers(struct ws_server *srv)
{
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT,
	    SIGSYS, SIGTRAP };
	struct sigaction sa;
	size_t i;

	(void)srv;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crash_handler;
	sa.sa_flags = SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);
}

int
vt_open(struct ws_server *srv)
{
	struct termios raw;
	struct vt_mode vm;
	char path[32];
	int ctl, vtno = -1;

	/*
	 * VT queries (VT_OPENQRY, VT_GETACTIVE, VT_ACTIVATE) need a vt(4)
	 * terminal: /dev/consolectl doesn't answer them. ttyv0 stays open so
	 * they keep working after the privilege drop.
	 */
	srv->vtctl_fd = open("/dev/ttyv0", O_RDWR | O_CLOEXEC | O_NOCTTY);
	ctl = srv->vtctl_fd;
	if (ctl < 0) {
		ws_err("can't open /dev/ttyv0: %s", strerror(errno));
		return -1;
	}
	if (ioctl(ctl, VT_GETACTIVE, &srv->vt_orig) < 0)
		srv->vt_orig = -1;
	if (srv->vt_num > 0)
		vtno = srv->vt_num;
	else if (ioctl(ctl, VT_OPENQRY, &vtno) < 0 || vtno <= 0) {
		ws_err("no free VT (VT_OPENQRY: %s)", strerror(errno));
		goto fail;
	}
	if (ctl != srv->vtctl_fd)
		close(ctl);

	/* vt(4) numbers VTs from 1; ttyv0 is VT 1. */
	snprintf(path, sizeof(path), "/dev/ttyv%x", vtno - 1);
	/*
	 * Non-blocking: in K_RAW mode keystrokes also reach this terminal, and
	 * vt_drain() reads until it's empty. A blocking read would then wait
	 * for the next key and stall the whole event loop.
	 */
	srv->tty_fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (srv->tty_fd < 0) {
		ws_err("open %s: %s", path, strerror(errno));
		return -1;
	}
	srv->vt_num = vtno;
	vt_saved.fd = srv->tty_fd;
	vt_saved.orig_vt = srv->vt_orig;

	if (ioctl(srv->tty_fd, VT_ACTIVATE, vtno) < 0 ||
	    ioctl(srv->tty_fd, VT_WAITACTIVE, vtno) < 0)
		ws_warn("switching to VT %d: %s", vtno, strerror(errno));

	memset(&vm, 0, sizeof(vm));
	vm.mode = VT_PROCESS;
	vm.relsig = VT_SIGNAL;
	vm.acqsig = VT_SIGNAL;
	vm.frsig = SIGIO;	/* unused, must be a valid signal */
	if (ioctl(srv->tty_fd, VT_SETMODE, &vm) < 0) {
		ws_err("VT_SETMODE: %s", strerror(errno));
		goto fail_restore;
	}
	if (ioctl(srv->tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
		ws_err("KDSETMODE KD_GRAPHICS: %s", strerror(errno));
		goto fail_restore;
	}
	if (tcgetattr(srv->tty_fd, &vt_saved.tios) == 0)
		vt_saved.have_tios = true;
	if (ioctl(srv->tty_fd, KDGKBMODE, &vt_saved.kbmode) == 0)
		vt_saved.have_kbmode = true;
	/* K_RAW keeps keys (and vt's own hotkeys) away from the console;
	 * libinput reads the keyboard, and Ctrl+Alt+Fn is ours to handle. */
	if (ioctl(srv->tty_fd, KDSKBMODE, K_RAW) < 0) {
		ws_err("KDSKBMODE K_RAW: %s", strerror(errno));
		goto fail_restore;
	}
	if (vt_saved.have_tios) {
		raw = vt_saved.tios;
		raw.c_iflag = IGNPAR | IGNBRK;
		raw.c_oflag = 0;
		raw.c_cflag = CREAD | CS8;
		raw.c_lflag = 0;
		raw.c_cc[VTIME] = 0;
		raw.c_cc[VMIN] = 1;
		cfsetispeed(&raw, 9600);
		cfsetospeed(&raw, 9600);
		tcsetattr(srv->tty_fd, TCSANOW, &raw);
	}
	vt_saved.active = true;
	srv->vt_setup = true;
	/* Raw scancodes still reach the tty; read and discard them. */
	ws_kq_add_fd(srv, srv->tty_fd, EVFILT_READ, (void *)(uintptr_t)SRC_TTY);
	ws_info("using VT %d (was on VT %d)", vtno, srv->vt_orig);
	return 0;

fail_restore:
	vt_restore();
	close(srv->tty_fd);
	srv->tty_fd = -1;
	vt_saved.fd = -1;
	return -1;
fail:
	if (ctl != srv->vtctl_fd)
		close(ctl);
	return -1;
}

void
vt_drain(struct ws_server *srv)
{
	char buf[256];

	while (read(srv->tty_fd, buf, sizeof(buf)) > 0)
		;
}

/* SIGUSR2 from vt(4): release when we're active, acquire when not. */
void
vt_signal(struct ws_server *srv)
{
	if (!srv->vt_setup)
		return;
	if (vt_saved.active) {
		ws_info("VT %d: releasing the display", srv->vt_num);
		keyboard_release_all(srv);
		input_suspend(srv);
		if (srv->output->suspend != NULL)
			srv->output->suspend(srv);
		srv->display_active = false;
		vt_saved.active = false;
		if (ioctl(srv->tty_fd, VT_RELDISP, 1) < 0)
			ws_warn("VT_RELDISP: %s", strerror(errno));
	} else {
		ws_info("VT %d: taking the display back", srv->vt_num);
		if (ioctl(srv->tty_fd, VT_RELDISP, VT_ACKACQ) < 0)
			ws_warn("VT_RELDISP ACKACQ: %s", strerror(errno));
		vt_saved.active = true;
		if (srv->output->resume != NULL && !srv->output->resume(srv))
			ws_err("couldn't take the display back");
		srv->display_active = true;
		srv->cursor_image = NULL;	/* reload the cursor */
		cursor_update(srv);
		input_resume(srv);
		damage_all(srv);
	}
}

void
vt_switch_to(struct ws_server *srv, int vt)
{
	if (!srv->vt_setup || vt == srv->vt_num)
		return;
	ws_info("switching to VT %d", vt);
	if (ioctl(srv->tty_fd, VT_ACTIVATE, vt) < 0)
		ws_warn("VT_ACTIVATE %d: %s", vt, strerror(errno));
}

void
vt_close(struct ws_server *srv)
{
	if (!srv->vt_setup)
		return;
	vt_restore();
	if (srv->vt_orig > 0 && srv->vt_orig != srv->vt_num)
		ioctl(srv->tty_fd, VT_ACTIVATE, srv->vt_orig);
	close(srv->tty_fd);
	srv->tty_fd = -1;
	vt_saved.fd = -1;
	if (srv->vtctl_fd >= 0)
		close(srv->vtctl_fd);
	srv->vtctl_fd = -1;
	srv->vt_setup = false;
}
