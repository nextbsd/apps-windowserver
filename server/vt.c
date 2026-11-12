/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Virtual terminal handling for DRM mode, following the same steps as
 * the NextBSD XLibre console patch (bsd_init.c: KD_GRAPHICS, K_RAW with
 * a raw termios, VT_SETMODE VT_PROCESS), written from scratch here.
 *
 * WindowServer doesn't open or switch VTs. It takes over the VT that is
 * active when it starts, and locks VT switching (VT_LOCKSWITCH) for as
 * long as it runs, so neither Ctrl+Alt+Fn nor `vidcontrol -s` nor any
 * other VT_ACTIVATE can take the screen away from it. VT_PROCESS mode is
 * set as well: it keeps kernel console output off the screen (vt(4)
 * marks the console unavailable while ttyv0 is in VT_PROCESS mode), and
 * should a switch request get through anyway, the server refuses it.
 *
 * On exit, a fatal signal or a crash the same VT gets back exactly what
 * it had: switching unlocked, its VT mode, KD_TEXT, its keyboard mode
 * and its termios. A SIGKILL can't be caught; stop the server with
 * SIGTERM.
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

/* VT_LOCKSWITCH arguments (vt_core.c: 1 locks, 2 unlocks). */
#define VT_LOCK_ON	1
#define VT_LOCK_OFF	2

/* Saved console state, also used by the crash handler. */
static struct {
	int		fd;
	int		kdmode;
	int		kbmode;
	struct termios	tios;
	struct vt_mode	vtmode;
	bool		have_tios;
	bool		have_kbmode;
	bool		have_vtmode;
	bool		locked;
} vt_saved = { .fd = -1 };

/* Only async-signal-safe calls (ioctl, tcsetattr): the crash handler
 * uses it too. */
static void
vt_restore(void)
{
	struct vt_mode vm;
	int fd = vt_saved.fd, lock = VT_LOCK_OFF;

	if (fd < 0)
		return;
	if (vt_saved.locked)
		ioctl(fd, VT_LOCKSWITCH, &lock);
	vt_saved.locked = false;
	ioctl(fd, KDSETMODE, vt_saved.kdmode);
	if (vt_saved.have_kbmode)
		ioctl(fd, KDSKBMODE, vt_saved.kbmode);
	if (vt_saved.have_tios)
		tcsetattr(fd, TCSANOW, &vt_saved.tios);
	/* The VT was in VT_AUTO when we took it (vt_open checks). */
	memset(&vm, 0, sizeof(vm));
	vm.mode = VT_AUTO;
	if (vt_saved.have_vtmode)
		vm = vt_saved.vtmode;
	ioctl(fd, VT_SETMODE, &vm);
}

static void
crash_handler(int sig)
{
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
	int ctl, vtno = -1, lock = VT_LOCK_ON;

	/*
	 * VT_GETACTIVE needs a vt(4) terminal: /dev/consolectl doesn't
	 * answer it. ttyv0 is only asked which VT is on screen.
	 */
	ctl = open("/dev/ttyv0", O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (ctl < 0) {
		ws_err("can't open /dev/ttyv0: %s", strerror(errno));
		return -1;
	}
	if (ioctl(ctl, VT_GETACTIVE, &vtno) < 0 || vtno <= 0) {
		ws_err("VT_GETACTIVE: %s", strerror(errno));
		close(ctl);
		return -1;
	}
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

	/* Someone else (Xorg, another WindowServer) already owns this VT. */
	if (ioctl(srv->tty_fd, KDGETMODE, &vt_saved.kdmode) < 0)
		vt_saved.kdmode = KD_TEXT;
	if (vt_saved.kdmode != KD_TEXT) {
		ws_err("VT %d is already in graphics mode; is Xorg or another "
		    "WindowServer running?", vtno);
		goto fail;
	}
	if (ioctl(srv->tty_fd, VT_GETMODE, &vt_saved.vtmode) == 0) {
		if (vt_saved.vtmode.mode == VT_PROCESS) {
			ws_err("VT %d is controlled by another process", vtno);
			goto fail;
		}
		vt_saved.have_vtmode = true;
	}
	if (tcgetattr(srv->tty_fd, &vt_saved.tios) == 0)
		vt_saved.have_tios = true;
	if (ioctl(srv->tty_fd, KDGKBMODE, &vt_saved.kbmode) == 0)
		vt_saved.have_kbmode = true;
	vt_saved.fd = srv->tty_fd;

	memset(&vm, 0, sizeof(vm));
	vm.mode = VT_PROCESS;
	vm.relsig = VT_SIGNAL;
	vm.acqsig = VT_SIGNAL;
	vm.frsig = SIGIO;	/* unused, must be a valid signal */
	if (ioctl(srv->tty_fd, VT_SETMODE, &vm) < 0) {
		ws_err("VT_SETMODE: %s", strerror(errno));
		goto fail_restore;
	}
	if (ioctl(srv->tty_fd, VT_LOCKSWITCH, &lock) < 0)
		ws_warn("VT_LOCKSWITCH: %s; switch requests will be refused",
		    strerror(errno));
	else
		vt_saved.locked = true;
	if (ioctl(srv->tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
		ws_err("KDSETMODE KD_GRAPHICS: %s", strerror(errno));
		goto fail_restore;
	}
	/* K_RAW keeps keys (and vt's own hotkeys) away from the console;
	 * libinput reads the keyboard. */
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
	srv->vt_setup = true;
	/* Raw scancodes still reach the tty; read and discard them. */
	ws_kq_add_fd(srv, srv->tty_fd, EVFILT_READ, (void *)(uintptr_t)SRC_TTY);
	ws_info("using VT %d, switching %s", vtno,
	    vt_saved.locked ? "locked" : "refused");
	return 0;

fail_restore:
	vt_restore();
fail:
	close(srv->tty_fd);
	srv->tty_fd = -1;
	vt_saved.fd = -1;
	return -1;
}

void
vt_drain(struct ws_server *srv)
{
	char buf[256];

	while (read(srv->tty_fd, buf, sizeof(buf)) > 0)
		;
}

/*
 * SIGUSR2 from vt(4): a request to switch away that got past the lock.
 * WindowServer keeps the display, so it says no. (An acquire signal
 * can't come, since the VT is never given up; acknowledge it anyway.)
 */
void
vt_signal(struct ws_server *srv)
{
	if (!srv->vt_setup)
		return;
	if (ioctl(srv->tty_fd, VT_RELDISP, VT_FALSE) == 0)
		ws_info("VT %d: refused a request to switch away", srv->vt_num);
	else if (ioctl(srv->tty_fd, VT_RELDISP, VT_ACKACQ) < 0)
		ws_debug("VT_RELDISP: %s", strerror(errno));
}

void
vt_close(struct ws_server *srv)
{
	if (!srv->vt_setup)
		return;
	vt_restore();
	close(srv->tty_fd);
	srv->tty_fd = -1;
	vt_saved.fd = -1;
	srv->vt_setup = false;
	ws_info("VT %d given back to the console", srv->vt_num);
}
