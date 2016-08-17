#
# Makefile for the OCR Runtime on the x86-linux platform
#
# For OCR licensing terms, see top level LICENSE file.
#
# Author: Ivan Ganev <ivan.b.ganev@intel.com>
#

ifndef OCR_TYPE
  OCR_TYPE=tg-xe
else
  ifneq (${OCR_TYPE}, tg-xe)
    $(error OCR_TYPE is set to ${OCR_TYPE} but expected tg-xe)
  endif
endif
#
# Tool-chain to be used for the build
#

DEFAULT_CONFIG = xe-default.cfg

TG_INSTALL ?= ../../../../tg/tg/install

PREFIX  := $(TG_INSTALL)/bin/xstg-linux-elf-
CC      := $(PREFIX)clang
CFLAGS  := -fxstg-extensions -fno-builtin -Wno-typedef-redefinition -I $(TG_INSTALL)/include

# Uncomment the following line to move the strings used in DPRINTF to another section
# which can then be moved to L2 or DRAM
CFLAGS  += -DOCR_DPRINTF_SECTION=.dprintf_str

RM      := rm
RMFLAGS := -rf

CP      := cp
MKDIR   := mkdir
LN      := ln

# Shared libraries specific builds
LDFLAGS :=

# CFLAGS_SHARED will be concatenated with any
# common CFLAGS options
CFLAGS_SHARED :=

# Static libraries specific builds
# Same as for CFLAGS_SHARED
CFLAGS_STATIC := $(CFLAGS)
AR := $(PREFIX)ar
ARFLAGS := cru

RANLIB := $(PREFIX)ranlib

# Library supported
SUPPORTS_SHARED=no
SUPPORTS_STATIC=yes

.PHONY: all
all: static

.PHONY: debug
debug: debug-static

include ../common.mk
