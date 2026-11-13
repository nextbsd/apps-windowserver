# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# ScreenCaptureAgent: WindowServer's Screen Recording consent agent, a
# small GNUstep app in CoreServices ("Screen capture" in WSProtocol.h).
# Needs gnustep-make, and libWindowServerClient (built first by the
# top-level gmake, in ../build, or installed):
#
#   gmake               (or gmake agent at the top)
#   gmake install       as root; DESTDIR=... works
#
# Installs ScreenCaptureAgent.app to /System/Library/CoreServices and its
# LaunchAgent to /System/Library/LaunchAgents.

ifeq ($(GNUSTEP_MAKEFILES),)
  GNUSTEP_MAKEFILES := $(shell gnustep-config --variable=GNUSTEP_MAKEFILES 2>/dev/null)
endif
ifeq ($(GNUSTEP_MAKEFILES),)
  $(error GNUSTEP_MAKEFILES is not set; source /System/Library/Makefiles/GNUstep.sh)
endif

# A system component: keep gnustep-make from creating Local domain dirs.
GNUSTEP_INSTALLATION_DOMAIN = SYSTEM

include $(GNUSTEP_MAKEFILES)/common.make

APP_NAME = ScreenCaptureAgent
ScreenCaptureAgent_INSTALL_DIR = $(DESTDIR)/System/Library/CoreServices
ScreenCaptureAgent_APPLICATION_ICON = ScreenCaptureAgent.png
ScreenCaptureAgent_RESOURCE_FILES = ScreenCaptureAgent.png

ScreenCaptureAgent_OBJC_FILES = \
	main.m \
	SCAgent.m

ScreenCaptureAgent_C_FILES = \
	SCLaunchd.c

# The headers and library of this repository's build, else the system's.
WS_BUILD ?= ../build
ADDITIONAL_CPPFLAGS += -I../include
ADDITIONAL_LIB_DIRS += -L$(WS_BUILD)
ADDITIONAL_OBJCFLAGS += -fobjc-arc -fblocks -Wall -Wno-import
ADDITIONAL_CFLAGS += -Wall
ADDITIONAL_GUI_LIBS += -lWindowServerClient -L/usr/lib/system -llaunch \
	-lsystem_kernel -Wl,-rpath,/usr/lib/system

LAUNCHAGENTS_DIR = $(DESTDIR)/System/Library/LaunchAgents

include $(GNUSTEP_MAKEFILES)/application.make

after-install::
	$(INSTALL) -d $(LAUNCHAGENTS_DIR)
	$(INSTALL) -m 644 org.nextbsd.ScreenCaptureAgent.plist $(LAUNCHAGENTS_DIR)/
