#
# Makefile for the OCR Runtime on the x86-linux platform
#
# For OCR licensing terms, see top level LICENSE file.
#
# Author: Ivan Ganev <ivan.b.ganev@intel.com>
#

ifndef OCR_TYPE
  OCR_TYPE=x86-phi-mpi
else
  ifneq (${OCR_TYPE}, x86-phi-mpi)
    $(error OCR_TYPE is set to ${OCR_TYPE} but expected x86-phi-mpi)
  endif
endif

#DEFAULT_CONFIG=jenkins-common-8w-lockableDB.cfg
DEFAULT_CONFIG=mach-x86-mpi-affinity-8w-lockableDB.cfg

#
# Tool-chain to be used for the build
#

# Careful with variables that are implicitely defined by make
# They may have a default value (CC, AR, ARFLAGS, RM).
# See make doc "Variables Used by Implicit Rules"

FORCE_CC ?= no
ifeq ($(FORCE_CC), no)
  ifeq ($(CC), cc)
    CC = mpiicc
  endif
endif # End of ifeq force_cc

# Conserve CFLAGS and LDFLAGS defined in environment
ifeq (${CC}, mpiicc)
CFLAGS  := ${CFLAGS} -xCOMMON-AVX512
endif
LDFLAGS := ${LDFLAGS}

RM      := rm
RMFLAGS := -rf

CP      := cp
MKDIR   := mkdir
LN      := ln

# Shared libraries specific builds
LDFLAGS += -shared -fpic -lpthread -lnuma

# -lrt needed only for < glibc-2.17 in Linux systems (not needed for OS X)
UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
  ret := $(shell echo "`ldd --version | awk '/ldd/{print $$NF}' | cut -d'.' -f1-2` < 2.17" | bc)
  ifeq ($(ret), 1)
    LDFLAGS += -lrt
  endif
endif

# CFLAGS_SHARED will be concatenated with any common CFLAGS options
CFLAGS_SHARED := ${CFLAGS_SHARED} -fpic

# Static libraries specific builds
# Same as for CFLAGS_SHARED
CFLAGS_STATIC := ${CFLAGS_STATIC}

AR := ar
ARFLAGS := cru

RANLIB := ranlib

# Library supported
SUPPORTS_SHARED=yes
SUPPORTS_STATIC=yes
OCRRUNNER=ocrrun_$(OCR_TYPE)

# Valgrind compatibility for internal allocators
# x86 only
# Requires valgrind-devel package
# CFLAGS += -I/usr/include -DENABLE_VALGRIND

# Runtime overhead profiler
# x86 only
#
# Enable profiler
# CFLAGS += -DOCR_RUNTIME_PROFILER -DPROFILER_KHZ=3400000
#
# (optional) Maximum number of scope
# nesting for runtime profiler
# CFLAGS += -DMAX_PROFILER_LEVEL=512

# Enables data collection for execution timeline visualizer
# x86 only
# Requires -DOCR_ENABLE_EDT_NAMING and DEBUG_LVL_INFO
# CFLAGS += -DOCR_ENABLE_VISUALIZER -DOCR_ENABLE_EDT_NAMING

# if MPI_ROOT is set, use the specified mpi install
ifdef MPI_ROOT
  # Name of the MPI shared library to link with
  MPI_LIB ?= mpi
  CFLAGS += -I${MPI_ROOT}/include
  LDFLAGS += -L${MPI_ROOT}/lib -l${MPI_LIB}
endif

.PHONY: all
all: static shared

.PHONY: debug
debug: debug-static debug-shared

include ../common.mk
