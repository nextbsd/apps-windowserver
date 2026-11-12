# Copyright (c) 2026, Joseph Maloney
# SPDX-License-Identifier: BSD-2-Clause
#
# WindowServer for NextBSD. Build with gmake:
#
#   gmake                 build everything into build/
#   gmake install         install (honours DESTDIR and the *DIR variables)
#   gmake check           run the headless tests (tests/headless-test.sh,
#                         tests/pointer-test.sh, tests/tiling-test.sh,
#                         tests/drag-test.sh)
#   gmake check-settings  the settings test against a running headless
#                         server (WS_SERVICE_NAME=..., tests/settings-test.sh)
#   gmake clean

CC		?= cc
PKG_CONFIG	?= pkgconf
INSTALL		?= install
LN		?= ln

DESTDIR		?=
PREFIX		?= /usr
# The server lives in CoreServices, beside loginwindow and the other system UI
# services. (macOS keeps it in a private framework; NextBSD has no such framework.)
SERVERDIR	?= /System/Library/CoreServices
LIBDIR		?= $(PREFIX)/lib
INCDIR		?= $(PREFIX)/include
PKGCONFIGDIR	?= $(PREFIX)/libdata/pkgconfig
TOOLDIR		?= $(PREFIX)/libexec/WindowServer
LAUNCHDDIR	?= /System/Library/LaunchDaemons
PREFSDIR	?= /System/Library/Preferences

# NextBSD's Mach and bootstrap libraries live in /usr/lib/system.
SYSLIBDIR	?= /usr/lib/system
MACH_LIBS	= -L$(SYSLIBDIR) -llaunch -lsystem_kernel -Wl,-rpath,$(SYSLIBDIR)

PKGS		= libdrm libinput libudev xkbcommon pixman-1
PKG_CFLAGS	:= $(shell $(PKG_CONFIG) --cflags $(PKGS))
PKG_LIBS	:= $(shell $(PKG_CONFIG) --libs $(PKGS))

WARN		= -Wall -Wextra -Wshadow -Wpointer-arith \
		  -Wstrict-prototypes -Wmissing-prototypes
CFLAGS		?= -O2 -g
CFLAGS		+= -std=c11 -D_DEFAULT_SOURCE $(WARN) -Iinclude
LDFLAGS		?=

LIB_MAJOR	= 1
LIB_NAME	= libWindowServerClient
LIB_SO		= $(LIB_NAME).so.$(LIB_MAJOR)

B		= build
SERVER_SRCS	= $(wildcard server/*.c)
SERVER_OBJS	= $(SERVER_SRCS:%.c=$(B)/%.o)
LIB_SRCS	= lib/wsclient.c
LIB_PIC_OBJS	= $(LIB_SRCS:%.c=$(B)/pic/%.o)
LIB_OBJS	= $(LIB_SRCS:%.c=$(B)/%.o)
HEADERS		= $(wildcard include/WindowServer/*.h)

all: $(B)/WindowServer $(B)/$(LIB_SO) $(B)/$(LIB_NAME).a $(B)/wstest $(B)/wsctl \
     $(B)/wspointer $(B)/wstiling $(B)/wsdrag \
     $(B)/WindowServerClient.pc

$(B)/server/%.o: server/%.c server/server.h $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -pthread -c -o $@ $<

$(B)/WindowServer: $(SERVER_OBJS)
	$(CC) $(LDFLAGS) -pthread -o $@ $(SERVER_OBJS) $(PKG_LIBS) $(MACH_LIBS) -lm

$(B)/lib/%.o: lib/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fvisibility=hidden -c -o $@ $<

$(B)/pic/lib/%.o: lib/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<

$(B)/$(LIB_NAME).a: $(LIB_OBJS)
	rm -f $@
	ar rcs $@ $(LIB_OBJS)

$(B)/$(LIB_SO): $(LIB_PIC_OBJS)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$(LIB_SO) -o $@ $(LIB_PIC_OBJS) $(MACH_LIBS)
	$(LN) -sf $(LIB_SO) $(B)/$(LIB_NAME).so

# Tools link the static library so they run from the build tree.
$(B)/tools/%.o: tools/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/wstest: $(B)/tools/wstest.o $(B)/$(LIB_NAME).a
	$(CC) $(LDFLAGS) -o $@ $< $(B)/$(LIB_NAME).a $(MACH_LIBS)

$(B)/wsctl: $(B)/tools/wsctl.o $(B)/$(LIB_NAME).a
	$(CC) $(LDFLAGS) -o $@ $< $(B)/$(LIB_NAME).a $(MACH_LIBS)

$(B)/wspointer: $(B)/tools/wspointer.o $(B)/$(LIB_NAME).a
	$(CC) $(LDFLAGS) -o $@ $< $(B)/$(LIB_NAME).a $(MACH_LIBS)

$(B)/wstiling: $(B)/tools/wstiling.o $(B)/$(LIB_NAME).a
	$(CC) $(LDFLAGS) -o $@ $< $(B)/$(LIB_NAME).a $(MACH_LIBS)

$(B)/wsdrag: $(B)/tools/wsdrag.o $(B)/$(LIB_NAME).a
	$(CC) $(LDFLAGS) -o $@ $< $(B)/$(LIB_NAME).a $(MACH_LIBS)

$(B)/WindowServerClient.pc: WindowServerClient.pc.in
	@mkdir -p $(B)
	# libdir and includedir are written relative to ${prefix} when they're
	# under it, so the file still works when the tree is moved or staged.
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@LIBDIR@|$(patsubst $(PREFIX)/%,$${prefix}/%,$(LIBDIR))|g' \
	    -e 's|@INCDIR@|$(patsubst $(PREFIX)/%,$${prefix}/%,$(INCDIR))|g' \
	    -e 's|@SYSLIBDIR@|$(SYSLIBDIR)|g' \
	    $< > $@

install: all
	$(INSTALL) -d $(DESTDIR)$(SERVERDIR) $(DESTDIR)$(LIBDIR) \
	    $(DESTDIR)$(INCDIR)/WindowServer $(DESTDIR)$(PKGCONFIGDIR) \
	    $(DESTDIR)$(TOOLDIR) $(DESTDIR)$(LAUNCHDDIR)
	$(INSTALL) -m 755 $(B)/WindowServer $(DESTDIR)$(SERVERDIR)/WindowServer
	$(INSTALL) -m 755 $(B)/$(LIB_SO) $(DESTDIR)$(LIBDIR)/$(LIB_SO)
	$(LN) -sf $(LIB_SO) $(DESTDIR)$(LIBDIR)/$(LIB_NAME).so
	$(INSTALL) -m 644 $(B)/$(LIB_NAME).a $(DESTDIR)$(LIBDIR)/$(LIB_NAME).a
	$(INSTALL) -m 644 $(HEADERS) $(DESTDIR)$(INCDIR)/WindowServer/
	$(INSTALL) -m 644 $(B)/WindowServerClient.pc $(DESTDIR)$(PKGCONFIGDIR)/
	$(INSTALL) -m 755 $(B)/wstest $(B)/wsctl $(B)/wspointer $(B)/wstiling \
	    $(B)/wsdrag \
	    scripts/ws-session \
	    $(DESTDIR)$(TOOLDIR)/
	$(INSTALL) -m 644 launchd/org.nextbsd.WindowServer.plist $(DESTDIR)$(LAUNCHDDIR)/
	@if [ ! -e $(DESTDIR)$(PREFSDIR)/org.nextbsd.WindowServer.plist ]; then \
	    $(INSTALL) -d $(DESTDIR)$(PREFSDIR); \
	    $(INSTALL) -m 644 config/org.nextbsd.WindowServer.plist $(DESTDIR)$(PREFSDIR)/; \
	fi

check: all
	sh tests/headless-test.sh $(B)
	sh tests/pointer-test.sh $(B)
	sh tests/tiling-test.sh $(B)
	sh tests/drag-test.sh $(B)

check-settings: all
	sh tests/settings-test.sh $(B)

clean:
	rm -rf $(B)

.PHONY: all install check check-settings clean
