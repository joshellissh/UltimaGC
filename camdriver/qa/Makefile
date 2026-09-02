# Makefile for camqa — the standalone V4L2 + DRM/KMS camera QA tool.
#
# Cross-compile friendly. It respects the standard Yocto SDK environment
# variables, so the normal flow is just:
#
#     . <sdk>/environment-setup-aarch64-oe-linux    # sets CC, CFLAGS, ...
#     make
#
# You can also drive it by hand:
#
#     make CROSS_COMPILE=aarch64-oe-linux- SYSROOT=/path/to/sysroot
#     make CC=aarch64-oe-linux-gcc SYSROOT=/path/to/sysroot
#
# Design notes:
#   * CC: GNU make predefines CC=cc with origin "default", so `CC ?= ...` would
#     NEVER fire and CROSS_COMPILE would be silently ignored. We use the kernel
#     pattern below: only override CC when its origin is "default". An SDK env
#     script exports CC with origin "environment", which correctly wins.
#   * libdrm headers: xf86drm.h does `#include <drm.h>`, which lives in
#     <prefix>/usr/include/libdrm/. We get the right -I from pkg-config when it
#     is available (the SDK env sets PKG_CONFIG up), and fall back to an explicit
#     -I$(SYSROOT)/usr/include/libdrm otherwise. <libdrm/drm_fourcc.h> is found
#     via the base sysroot include path.

CROSS_COMPILE ?=

# Only set CC from CROSS_COMPILE if CC is still make's built-in default.
ifeq ($(origin CC),default)
CC = $(CROSS_COMPILE)gcc
endif

TARGET := camqa
SRC    := camqa.c

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra

# --sysroot so headers/libs resolve against the target when SYSROOT is set.
ifneq ($(strip $(SYSROOT)),)
CFLAGS  += --sysroot=$(SYSROOT)
LDFLAGS += --sysroot=$(SYSROOT)
endif

# Prefer pkg-config for libdrm's include/lib flags; fall back to a manual -I
# against the sysroot plus a plain -ldrm.
PKG_CONFIG ?= pkg-config
DRM_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null)
DRM_LIBS   := $(shell $(PKG_CONFIG) --libs   libdrm 2>/dev/null)
ifeq ($(strip $(DRM_LIBS)),)
DRM_LIBS := -ldrm
ifneq ($(strip $(SYSROOT)),)
DRM_CFLAGS := -I$(SYSROOT)/usr/include/libdrm
endif
endif

CFLAGS += $(DRM_CFLAGS)
LDLIBS += $(DRM_LIBS)

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
