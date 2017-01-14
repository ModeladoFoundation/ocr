#
# OCR top level directory
#
OCR_ROOT ?= ../..
OCR_INSTALL ?= ../../install

OCR_BUILD ?= .
#
# Object & dependence file subdirectory
#

#
# Default machine configuration
#
DEFAULT_CONFIG ?= jenkins-common-8w-lockableDB.cfg

####################################################
# User Configurable settings
####################################################

# for jenkins testing purpose
#CFLAGS += -DUTASK_COMM -DUTASK_COMM2

# **** System-dependent values ****

# Cache line size in bytes (Mostly used for padding)
CFLAGS += -DCACHE_LINE_SZB=64

# **** Lock implementation ****

# By default, a test test-and-set lock is used but you
# can also enable a ticket-lock instead. This increase space
# requirements for each lock (from 32 bits to 64 bits)
#CFLAGS += -DOCR_TICKETLOCK

# You can optionally set a back-off for the ticket lock. This
# is a proportional backoff and should be roughly the lenght
# of critical sections. By default, there is no back-off (value of 0)
#CFLAGS += -DOCR_TICKETLOCK_BACKOFF=0

# **** Runtime extension parameters (ENABLE_EXTENSION_RTITF) ****

# Number of elements in EDT local storage
# CFLAGS += -DELS_USER_SIZE=0

# **** Deferred Execution Model ****

# CFLAGS += -DENABLE_OCR_API_DEFERRABLE

# **** Workpiles Parameters ****

# Impl-specific for Work-stealing deques
# - Static size for deques used to contain EDTs
# CFLAGS += -DINIT_DEQUE_CAPACITY=2048

# **** Registration Parameters ****

# Two-steps asynchronous registration
# CFLAGS += -DREG_ASYNC

# Single-steps asynchronous registration
# CFLAGS += -DREG_ASYNC_SGL

# **** Events Parameters ****

# Initialisation size for statically allocated HC event's waiter array
# CFLAGS += -DHCEVT_WAITER_STATIC_COUNT=4

# Initialisation size for dynamically allocated HC event's waiter array
# CFLAGS += -DHCEVT_WAITER_DYNAMIC_COUNT=4

# Enable MetaData Cloning for events
# CFLAGS += -DENABLE_EVENT_MDC

# Enable forging event's instance when MetaData Cloning is on
# i.e. do not communicate with the event's owner but rather
# create a copy immediately and sync-up later.
# CFLAGS += -DENABLE_EVENT_MDC_FORGE

# [Experimental flag] Make all Channel Events non-FIFO
# CFLAGS += -DXP_CHANNEL_EVT_NONFIFO

# **** GUID-Provider Parameters ****

# All impl-specific for counted-map and labeled-guid providers

# - Controls how many bits in the GUID are used for PD location
CFLAGS += -DGUID_PROVIDER_LOCID_SIZE=10

# - Per worker GUID generation: GUID_WID_SIZE is the maximum
#   number of workers supported per PD
# CFLAGS += -DGUID_PROVIDER_WID_INGUID += -DGUID_WID_SIZE=4

# - Activate a different hashmap implementation
#   Warning: Necessitates an additional -D activating the alternate implementation
# CFLAGS += -DGUID_PROVIDER_CUSTOM_MAP -D_TODO_FILL_ME_IN

# **** Hashtable Parameters ****

# - Distribute hashtable locks over cache lines
# CFLAGS += -DHASHTABLE_LOCK_SPREAD

# - Activates hashtable statistics
#   - Prints high watermark on buckets
# CFLAGS += -DSTATS_HASHTABLE

# - Keeps track of bucket's lock collision
# CFLAGS += -DSTATS_HASHTABLE_COLLIDE

# - Print per bucket stats
# CFLAGS += -DSTATS_HASHTABLE_VERB

# **** Communication Platform Parameters ****

# - MPI specifics
#   - Make use of MPI-3 message
# CFLAGS += -DMPI_MSG
#   - For MPI_PROBE impl, push new operations at
#     the tail of incoming and outgoing queues
# CFLAGS += -DMPI_COMM_PUSH_AT_TAIL
#   - Forces to allocate MPI requests instead of using pool
# CFLAGS += -DMPI_ALLOC_REQ

# **** EDTs parameters ****

# Maximum number of blocks of 64 slots that an EDT
# can have IFF it needs to acquire the same DB on
# multiple slots
# CFLAGS += -DOCR_MAX_MULTI_SLOT=1

# **** Debugging parameters ****

# Maximum number of characters handled by a single PRINTF
# CFLAGS += -DPRINTF_MAX=1024

# Enables naming of EDTs for easier debugging
# CFLAGS += -DOCR_ENABLE_EDT_NAMING

# For EDT naming, defines the maximum number of characters
# for the name (defaults to 32 including '\0')
# CFLAGS += -DOCR_EDT_NAME_SIZE=32

####################################################
# Platform specific user configurable settings
#
# Set these in <platform>/Makefile
# They are added here for reference
###################################################

# Debug and Nanny mode

# Valgrind compatibility for internal allocators
# x86 only
# Requires valgrind-devel package
# CFLAGS += -I/usr/include -DENABLE_VALGRIND

# Bypass runtime allocators in favor of standard malloc
# CFLAGS += -DNANNYMODE_SYSALLOC

# Declare flags for AddressSanitizer
# Warning: Applications must use the same flags else it will crash.
ifeq (${OCR_ASAN}, yes)
  ASAN_FLAGS := -g -fsanitize=address -fno-omit-frame-pointer
  CFLAGS += $(ASAN_FLAGS)
  LDFLAGS += $(ASAN_FLAGS) $(LDFLAGS)
endif

# Extrae instrumentation
# x86 only
#
# Enable instrumentation
# CFLAGS += -DEXTRAE_RUNTIME_INSTRUMENTATION
#
# Extrae header and library flags
# CFLAGS += -I$(EXTRAE_HOME)/include
# LDFLAGS := -L$(EXTRAE_HOME)/lib -lpttrace $(LDFLAGS)
#
# Select events to be instrumented
# 'HWC' version of the flag reads
# hardware counter values on event creation.
#
# Instrument scheduler events
# CFLAGS += -DENABLE_EVENT_SCHED
# CFLAGS += -DENABLE_HWC_EVENT_SCHED
#
# Instrument user code
# CFLAGS += -DENABLE_EVENT_USERCODE
# CFLAGS += -DENABLE_HWC_EVENT_USERCODE
#
# Instrument (wo?)
# CFLAGS += -DENABLE_EVENT_WO
# CFLAGS += -DENABLE_HWC_EVENT_WO
#
# Instrument API
# CFLAGS += -DENABLE_EVENT_API
# CFLAGS += -DENABLE_HWC_EVENT_API
#
# Instrument Policy Domain
# CFLAGS += -DENABLE_EVENT_PD
# CFLAGS += -DENABLE_HWC_EVENT_PD
#
# Instrument (cp?)
# CFLAGS += -DENABLE_EVENT_CP
# CFLAGS += -DENABLE_HWC_EVENT_CP
#
# Instrument (ta?)
# CFLAGS += -DENABLE_EVENT_TA
# CFLAGS += -DENABLE_HWC_EVENT_TA
#
#
#
# Runtime overhead profiler
# x86 only
#
# Enable profiler
# CFLAGS += -DOCR_RUNTIME_PROFILER -DPROFILER_KHZ=3400000
#
# Other options for the profiler. All are disabled by default
# Enable this if you want to use the profiler in your app as well
# CFLAGS += -DPROFILER_W_APPS
#
# Enable this if you want to set a symbol from which
# to start profiling (typically: "enter into user code")
# CFLAGS += -DPROFILER_FOCUS=userCode
#
# The following option is only relevant with PROFILER_FOCUS
# Enable this if you want the profiler to stop giving details
# after entering this many levels of profiler "stack". For example
# if you have no profiler calls in your apps and want to determine
# the overhead of the runtime from your user code, set this to 1 and
# PROFILER_FOCUS to userCode. The profiler will report the time spent
# in userCode as well as the time spent in each OCR API call but nothing
# else
# CFLAGS += -DPROFILER_FOCUS_DEPTH=1
#
# The following option is only relevant with PROFILER_FOCUS
# Enable this if you want to stop profiling when a runtime function call
# is made. This will override FOCUS_DEPTH (ie: if a runtime call is encountered
# before FOCUS_DEPTH is reached, the profiler will stop giving details of sub-calls)
# CFLAGS += -DPROFILER_IGNORE_RT
#
# The following option is only relevant with PROFILER_FOCUS
# Enable this if you want to gather everything that happens outside of the
# focus function into an EVENT_OTHER bucket.
# CFLAGS += -DPROFILER_COUNT_OTHER
#
# The following option is only relevant with PROFILER_FOCUS
# By default, this is equal to PROFILER_FOCUS. The first level of
# runtime calls from the PROFILER_PEEK function will be printed.
# CFLAGS += -DPROFILER_PEEK=userCode
#
# The following is only relevant with PROFILER_FOCUS and PROFILER_IGNORE_RT
# You can override what you consider to be runtime calls (and ignored by
# PROFILER_IGNORE_RT) and user calls. By default, all runtime calls
# are considered as runtime calls and user calls as user calls but you can
# change that by setting PROFILER_NAME_ISRT to either 1 (to consider as
# a runtime call) or 0 otherwise. NAME should be the name of the call, for
# example PROFILER_userCode_ISRT. Note that by default, the calls PROFILER_FOCUS
# and PROFILER_PEEK are always considered as user calls
# CFLAGS += -DPROFILER_userCode_ISRT=0
#
# (optional) Maximum number of scope nesting for runtime profiler
# CFLAGS += -DMAX_PROFILER_LEVEL=512

# Enables data collection for execution timeline visualizer
# x86 only
# Requires -DOCR_ENABLE_EDT_NAMING and DEBUG_LVL_INFO
# CFLAGS += -DOCR_ENABLE_VISUALIZER -DOCR_ENABLE_EDT_NAMING

# Enable custom tracing to be written to binary neglecting console output
# (Primarily for LLNL tools inter-operability)
# CFLAGS += -DOCR_TRACE_BINARY

# Enable monitoring/logging of message traffic between policy domains
# Requires Tracing (-DOCR_TRACE_BINARY)
# CFLAGS += -DOCR_MONITOR_NETWORK -DOCR_TRACE_BINARY

# Enable trace events for monitoring scheduling overhead activity
# Requires OCR_TRACE_BINARY
# CFLAGS += -DOCR_MONITOR_SCHEDULER -DOCR_TRACE_BINARY

# Enable custom tracing for collecting trace data for OCR simulator
# Requires OCR_TRACE_BINARY
# CFLAGS += -DOCR_ENABLE_SIMULATOR -DOCR_TRACE_BINARY


# **** Hacks ****

## Hacks related to memory movement (TG only)
# Define whether or not DBs move on TG
CFLAGS += -DOCR_HACK_DB_MOVE
# Define whether or not L1 is reserved for RT objects on the XEs
CFLAGS += -DOCR_HACK_L1_RT_ONLY
# Define whether or not user datablocks are pushed to L3 and beyond
CFLAGS += -DOCR_HACK_L3_DBS

# Set to WARN if you want to get them
CFLAGS += -DOCR_HACK_DEBUG_LVL=DEBUG_LVL_VERB

####################################################
# Experimental flags
####################################################

# Enable naming of EDTs
# This uses the name of the function to name the EDT templates and therefore
# the EDTs.
# NOTE: The application must also have this flag defined
# If this is note the case, an ASSERT will happen
# CFLAGS += -DOCR_ENABLE_EDT_NAMING

# Flag to test 128-bit guids.
# CFLAGS += -DOCR_ENABLE_128_BIT_GUID

####################################################
# Debug flags
####################################################

# Debugging support
ifneq (${NO_DEBUG}, yes)
  # Enable assertions
  CFLAGS += -DOCR_ASSERT
  # Enable debug
  CFLAGS += -DOCR_DEBUG
  OPT_LEVEL=-O2
else
  OPT_LEVEL=-O3
endif

# Turns on critical asserts that are always checked
#CFLAGS += -DOCR_ASSERT_CRITICAL

# Define level
CFLAGS += -DOCR_DEBUG_LVL=DEBUG_LVL_WARN
# CFLAGS += -DOCR_DEBUG_LVL=DEBUG_LVL_INFO
# CFLAGS += -DOCR_DEBUG_LVL=DEBUG_LVL_VERB
# CFLAGS += -DOCR_DEBUG_LVL=DEBUG_LVL_VVERB

# Define which modules you want for debugging
# You can optionally define an individual debuging level by
# defining DEBUG_LVL_XXX like OCR_DEBUG_LEVEL. If not defined,
# the default will be used
CFLAGS += -DOCR_DEBUG_API
# This next line shows how you can override the debug level for an
# individual module, either raising or lowering it
CFLAGS += -DOCR_DEBUG_ALLOCATOR
CFLAGS += -DOCR_DEBUG_COMP_PLATFORM
CFLAGS += -DOCR_DEBUG_COMM_PLATFORM #-DDEBUG_LVL_COMM_PLATFORM=DEBUG_LVL_VVERB
CFLAGS += -DOCR_DEBUG_COMM_API
CFLAGS += -DOCR_DEBUG_COMM_WORKER
CFLAGS += -DOCR_DEBUG_COMP_TARGET
CFLAGS += -DOCR_DEBUG_DATABLOCK
CFLAGS += -DOCR_DEBUG_EVENT
CFLAGS += -DOCR_DEBUG_GUID
CFLAGS += -DOCR_DEBUG_INIPARSING
CFLAGS += -DOCR_DEBUG_MACHINE
CFLAGS += -DOCR_DEBUG_MEM_PLATFORM
CFLAGS += -DOCR_DEBUG_MEM_TARGET
CFLAGS += -DOCR_DEBUG_POLICY
CFLAGS += -DOCR_DEBUG_MICROTASKS #-DDEBUG_LVL_MICROTASKS=DEBUG_LVL_VVERB
CFLAGS += -DOCR_DEBUG_SAL
CFLAGS += -DOCR_DEBUG_SCHEDULER
CFLAGS += -DOCR_DEBUG_SCHEDULER_HEURISTIC
CFLAGS += -DOCR_DEBUG_SCHEDULER_OBJECT
CFLAGS += -DOCR_DEBUG_STATS
CFLAGS += -DOCR_DEBUG_SYNC
CFLAGS += -DOCR_DEBUG_SYSBOOT
CFLAGS += -DOCR_DEBUG_TASK
CFLAGS += -DOCR_DEBUG_UTIL -DDEBUG_LVL_UTIL=DEBUG_LVL_WARN
CFLAGS += -DOCR_DEBUG_WORKER
CFLAGS += -DOCR_DEBUG_WORKPILE

# Tracing support
# Tracing prints minimal tracing information
# CFLAGS += -DOCR_TRACE

# Each module can individually be traced
CFLAGS += -DOCR_TRACE_API
CFLAGS += -DOCR_TRACE_ALLOCATOR
CFLAGS += -DOCR_TRACE_COMP_PLATFORM
CFLAGS += -DOCR_TRACE_COMM_PLATFORM
CFLAGS += -DOCR_TRACE_COMM_API
CFLAGS += -DOCR_TRACE_COMM_WORKER
CFLAGS += -DOCR_TRACE_COMP_TARGET
CFLAGS += -DOCR_TRACE_DATABLOCK
CFLAGS += -DOCR_TRACE_EVENT
CFLAGS += -DOCR_TRACE_GUID
CFLAGS += -DOCR_TRACE_INIPARSING
CFLAGS += -DOCR_TRACE_MACHINE
CFLAGS += -DOCR_TRACE_MEM_PLATFORM
CFLAGS += -DOCR_TRACE_MEM_TARGET
CFLAGS += -DOCR_TRACE_POLICY
CFLAGS += -DOCR_TRACE_SCHEDULER
CFLAGS += -DOCR_TRACE_SCHEDULER_HEURISTIC
CFLAGS += -DOCR_TRACE_SCHEDULER_OBJECT
CFLAGS += -DOCR_TRACE_STATS
CFLAGS += -DOCR_TRACE_SYNC
CFLAGS += -DOCR_TRACE_SYSBOOT
CFLAGS += -DOCR_TRACE_TASK
CFLAGS += -DOCR_TRACE_UTIL
CFLAGS += -DOCR_TRACE_WORKER
CFLAGS += -DOCR_TRACE_WORKPILE

# The following four flags can be used to help diagnose malfunctions
# in the dynamic memory allocators and/or in the code that utilizes
# dynamic memory blocks.  Such things as writing beyond the bounds of
# a dynamic datablock or de-referencing a pointer to a datablock that
# has been freed may be easier to spot with these flags switched on.
#
# On execution environments where valgrind is available, that is a much
# better starting point, but utilizing that plus the LEAK feature will
# be a more sure-footed way of finding de-referencing of stale pointers.
#
# These flags are utilized in TLSF and in mallocProxy, and will hopefully
# be utilized by other allocators as they are implemented, but that is not
# promised nor a fundamental requirement.
#
# Enable or disable checksum utilization:
#CFLAGS += -DENABLE_ALLOCATOR_CHECKSUM
# Disable or set to u64 value to broadcast across payloads of new datablocks:
#CFLAGS += -DENABLE_ALLOCATOR_INIT_NEW_DB_PAYLOAD=0xDad0fFae11111111LL
# Disable or set to u64 value to broadcast across payloads of DBs being freed:
#CFLAGS += -DENABLE_ALLOCATOR_TRASH_FREED_DB_PAYLOAD=0xDad0fFae22222222LL
# Enable or disable the on-purpose "leaking" of freed datablocks:
#CFLAGS += -DENABLE_ALLOCATOR_LEAK_FREED_DATABLOCKS

#
# Global CFLAGS to be passed into all architecture builds
# concatenated with the architecture-specific CFLAGS at the end

CFLAGS := -g -Wall $(CFLAGS) $(CFLAGS_USER)

################################################################
# END OF USER CONFIGURABLE OPTIONS                             #
################################################################


# Make sure that incompletely produced files are deleted
# on error
.DELETE_ON_ERROR:

#
# Make sure we have absolute paths
#
ifeq (,$(OCR_ROOT))
  $(error OCR_ROOT needs to be defined)
else
  _T := $(OCR_ROOT)
  OCR_ROOT := $(realpath $(_T))
  ifeq (,$(OCR_ROOT))
    $(error OCR_ROOT is not a valid path: $(_T))
  endif
endif

ifeq (,$(OCR_BUILD))
  $(error OCR_BUILD needs to be defined)
else
  _T := $(OCR_BUILD)
  OCR_BUILD := $(realpath $(_T))
  ifeq (,$(OCR_BUILD))
    $(info Creating OCR_BUILD: $(_T))
    $(shell mkdir -p "$(_T)")
    OCR_BUILD := $(realpath $(_T))
  endif
endif

ifeq (,$(OCR_INSTALL))
  $(error OCR_INSTALL needs to be defined)
else
  _T := $(OCR_INSTALL)
  OCR_INSTALL := $(realpath $(_T))
  ifeq (,$(OCR_INSTALL))
    $(info Creating OCR_INSTALL: $(_T))
    $(shell mkdir -p "$(_T)")
    OCR_INSTALL := $(realpath $(_T))
  endif
endif

# Verbosity of make
V ?= 0
AT_0 := @
AT_1 :=
AT = $(AT_$(V))

I ?= 0
ATI_0 := @
ATI_1 :=
ATI = $(ATI_$(I))

#
# Object & dependence file subdirectory
#
OBJDIR := $(OCR_BUILD)/objs

#
# Generate a list of all source files and the respective objects
#
SRCS   := $(shell find -L $(OCR_ROOT)/src -name '*.[csS]' -print)

#
# Generate a source search path
#
VPATH  := $(shell find -L $(OCR_ROOT)/src -type d -print)

ifneq (,$(findstring EXTRAE_RUNTIME_INSTRUMENTATION,$(CFLAGS)))
  INSTRUMENTATION_FILE=$(OCR_BUILD)/src/instrumentationAutoGenRT.h
  CFLAGS += -I $(OCR_BUILD)/src
  VPATH += $(OCR_BUILD)/src
endif

ifneq (,$(findstring OCR_RUNTIME_PROFILER,$(CFLAGS)))
  SRCSORIG := $(SRCS)
  PROFILER_FILE_C=$(OCR_BUILD)/src/profilerAutoGen.c
  SRCS += $(PROFILER_FILE_C)
  PROFILER_FILE=$(OCR_BUILD)/src/profilerAutoGenRT.h
  CFLAGS += -I $(OCR_BUILD)/src
  VPATH += $(OCR_BUILD)/src

  ifneq (,$(findstring PROFILER_W_APPS, $(CFLAGS)))
    PROFILER_MODE := rtapp
  else
    PROFILER_MODE := rt
  endif

  ifneq (,$(findstring PROFILER_COUNT_OTHER, $(CFLAGS)))
    PROFILER_EXTRA_OPTS := --otherbucket
  else
    PROFILER_EXTRA_OPTS :=
  endif

  ifeq ($(I), 1)
    $(info Profiler support turned on in mode $(PROFILER_MODE) with options "$(PROFILER_EXTRA_OPTS)")
  endif
else
  PROFILER_FILE   :=
  PROFILER_FILE_C :=
endif

ifeq ($(I), 1)
  $(info OCR_BUILD   is: $(OCR_BUILD))
  $(info OCR_ROOT    is: $(OCR_ROOT))
  $(info OCR_INSTALL is: $(OCR_INSTALL))
endif

OBJS_STATIC   := $(addprefix $(OBJDIR)/static/, $(addsuffix .o, $(basename $(notdir $(SRCS)))))
OBJS_SHARED   := $(addprefix $(OBJDIR)/shared/, $(addsuffix .o, $(basename $(notdir $(SRCS)))))
OBJS_EXEC     := $(addprefix $(OBJDIR)/exec/, $(addsuffix .o, $(basename $(notdir $(SRCS)))))

# Update include paths
CFLAGS := -I . -I $(OCR_ROOT)/inc -I $(OCR_ROOT)/src -I $(OCR_ROOT)/src/inc $(CFLAGS)

ifneq (${NO_DEBUG}, yes)
  ifeq (, $(filter-out gcc mpicc, $(CC)))
    # For gcc/mpicc versions < 4.4 disable warning as error as message pragmas are not supported
    ret := $(shell echo "`$(CC) -dumpversion | cut -d'.' -f1-2` < 4.4" | bc)
    ifeq ($(ret), 0)
      CFLAGS += -Werror
    endif
  else
    CFLAGS += -Werror
  endif
endif

# Static library name (only set if not set in OCR_TYPE specific file)
ifeq (${SUPPORTS_STATIC}, yes)
  OCRSTATIC ?= libocr_${OCR_TYPE}.a
  OCRSTATIC := $(OCR_BUILD)/$(OCRSTATIC)
  CFLAGS_STATIC ?=
  CFLAGS_STATIC := ${CFLAGS} ${CFLAGS_STATIC}
endif
# Shared library name (only set if not set in OCR_TYPE specific file)
ifeq (${SUPPORTS_SHARED}, yes)
  CFLAGS_SHARED ?=
  CFLAGS_SHARED := ${CFLAGS} ${CFLAGS_SHARED}
  OCRSHARED ?= libocr_${OCR_TYPE}.so
  OCRSHARED := $(OCR_BUILD)/$(OCRSHARED)
endif
# Executable name (only set if not set in OCR_TYPE specific file)
ifeq (${SUPPORTS_EXEC}, yes)
  CFLAGS_EXEC ?=
  CFLAGS_EXEC := ${CFLAGS} ${CFLAGS_EXEC}
  OCREXEC ?= ocrBuilder_$(OCR_TYPE).exe
  OCREXEC := $(OCR_BUILD)/$(OCREXEC)
endif

#
# Platform specific options
#
# Removed for now as this does not work with all
# versions of Apple's ranlib
# This is to remove the warnings when building the library
#ifeq ($(shell $(RANLIB) -V 2>/dev/null | head -1 | cut -f 1 -d ' '), Apple)
#  RANLIB := $(RANLIB) -no_warning_for_no_symbols
#  ARFLAGS := cruS
#endif

#
# Build targets
#

.PHONY: static
static: CFLAGS_STATIC += $(OPT_LEVEL)
static: supports-static info-static $(OCRSTATIC)

.PHONY: shared
shared: CFLAGS_SHARED += $(OPT_LEVEL)
shared: supports-shared info-shared $(OCRSHARED)

.PHONY: exec
exec: CFLAGS_EXEC += $(OPT_LEVEL)
exec: supports-exec info-exec $(OCREXEC)

.PHONY: debug-static
debug-static: CFLAGS_STATIC += -O0
debug-static: supports-static info-static $(OCRSTATIC)

.PHONY: debug-shared
debug-shared: CFLAGS_SHARED += -O0
debug-shared: supports-shared info-shared $(OCRSHARED)

.PHONY: debug-exec
debug-exec: CFLAGS_EXEC += -O0
debug-exec: supports-exec info-exec $(OCREXEC)

# This is for the profiler-generated file
$(OCR_BUILD)/src:
	$(AT)$(MKDIR) -p $(OCR_BUILD)/src

# Static target

.PHONY: supports-static
supports-static:
ifneq (${SUPPORTS_STATIC}, yes)
	$(error OCR type ${OCR_TYPE} does not support static library building)
endif

${OBJDIR}/static:
	$(AT)$(MKDIR) -p $(OBJDIR)/static


.PHONY: info-static
info-static:
	@printf "\033[32m>>>> Compile command for .c files is\033[1;30m '$(CC) $(CFLAGS_STATIC) -MMD -c <src> -o <obj>'\033[0m\n"
	@printf "\033[32m>>>> Building a static library with\033[1;30m '$(AR) $(ARFLAGS)'\033[0m\n"

$(OCRSTATIC): $(OBJS_STATIC)
	@echo "Linking static library ${OCRSTATIC}"
	$(AT)$(AR) $(ARFLAGS) $(OCRSTATIC) $^
	$(AT)$(RANLIB) $(OCRSTATIC)


# Shared target
.PHONY: supports-shared
supports-shared:
ifneq (${SUPPORTS_SHARED}, yes)
	$(error OCR type ${OCR_TYPE} does not support shared library building)
endif

${OBJDIR}/shared:
	$(AT)$(MKDIR) -p ${OBJDIR}/shared

.PHONY: info-shared
info-shared:
	@printf "\033[32m>>>> Compile command for .c files is\033[1;30m '$(CC) $(CFLAGS_SHARED) -MMD -c <src> -o <obj>'\033[0m\n"
	@printf "\033[32m>>>> Building a shared library with\033[1;30m '$(CC) $(LDFLAGS)'\033[0m\n"

$(OCRSHARED): $(OBJS_SHARED)
	@echo "Linking shared library ${OCRSHARED}"
	$(AT)$(CC) $(LDFLAGS) -o $(OCRSHARED) $^

# Exec target

.PHONY: supports-exec
supports-exec:
ifneq (${SUPPORTS_EXEC}, yes)
	$(error OCR type ${OCR_TYPE} does not support executable binary building)
endif

${OBJDIR}/exec:
	$(AT)$(MKDIR) -p $(OBJDIR)/exec


.PHONY: info-exec
info-exec:
	@printf "\033[32m>>>> Compile command for .c files is\033[1;30m '$(CC) $(CFLAGS_EXEC) -MMD -c <src> -o <obj>'\033[0m\n"

$(OCREXEC): $(OBJS_EXEC)
	@echo "Linking executable binary ${OCREXEC}"
	$(AT)$(CC) -o $(OCREXEC) $^ $(EXEFLAGS)


#
# Objects build rules
#
$(INSTRUMENTATION_FILE):
	@echo "Generating instrumentation header file..."
	$(AT)$(OCR_ROOT)/scripts/Profiler/generateInstrumentationFile.py -m rt -o $(OCR_BUILD)/src/instrumentationAutoGen --exclude .git --exclude profiler $(PROFILER_EXTRA_OPTS) $(OCR_ROOT)/src
	@echo "\tDone."

$(PROFILER_FILE): $(SRCSORIG) | $(OCR_BUILD)/src
	@echo "Generating profile file..."
	$(AT)$(OCR_ROOT)/scripts/Profiler/generateProfilerFile.py -m $(PROFILER_MODE) -o $(OCR_BUILD)/src/profilerAutoGen --exclude .git --exclude profiler $(PROFILER_EXTRA_OPTS) $(OCR_ROOT)/src
	@echo "\tDone."

$(PROFILER_FILE_C): $(PROFILER_FILE)

# This does a more complicate dependency computation so all the prereqs listed
# will also become command-less, prereq-less targets. This causes make
# to remake anything that depends on that file which is
# exactly what we want. This is adapted from:
# http://scottmcpeak.com/autodepend/autodepend.html
# Other information on some of the other rules for dependences can be found
# here: http://make.mad-scientist.net/papers/advanced-auto-dependency-generation/
#   sed:    strip the target (everything before colon)
#   sed:    remove any continuation backslashes
#   fmt -1: list words one per line
#   sed:    strip leading spaces
#   sed:    add trailing colon
#   touch:  sets the time to the same as the .o to avoid
#           erroneous detection next time around

# Delete default rules so it makes sure to use ours
%.o: %.c

$(OBJDIR)/static/%.o: %.c Makefile ../common.mk $(PROFILER_FILE) $(INSTRUMENTATION_FILE) $(OBJDIR)/static/%.d | $(OBJDIR)/static
	@echo "Compiling $<"
	$(AT)$(CC) $(CFLAGS_STATIC) -MMD -c $< -o $@
	$(AT)cp -f $(@:.o=.d) $(@:.o=.d.tmp)
	$(AT)sed -e 's/.*://' -e 's/\\$$//' < $(@:.o=.d.tmp) | fmt -1 | \
	sed -e 's/^ *//' -e 's/$$/: /' >> $(@:.o=.d)
	$(AT)touch -r $@ $(@:.o=.d)
	$(AT)rm -f $(@:.o=.d.tmp)


$(OBJDIR)/shared/%.o: %.c Makefile ../common.mk $(PROFILER_FILE) $(INSTRUMENTATION_FILE) $(OBJDIR)/shared/%.d | $(OBJDIR)/shared
	@echo "Compiling $<"
	$(AT)$(CC) $(CFLAGS_SHARED) -MMD -c $< -o $@
	$(AT)cp -f $(@:.o=.d) $(@:.o=.d.tmp)
	$(AT)sed -e 's/.*://' -e 's/\\$$//' < $(@:.o=.d.tmp) | fmt -1 | \
	sed -e 's/^ *//' -e 's/$$/: /' >> $(@:.o=.d)
	$(AT)touch -r $@ $(@:.o=.d)
	$(AT)rm -f $(@:.o=.d.tmp)

$(OBJDIR)/exec/%.o: %.c Makefile ../common.mk $(OBJDIR)/exec/%.d | $(OBJDIR)/exec
	@echo "Compiling $<"
	$(AT)$(CC) $(CFLAGS_EXEC) -MMD -c $< -o $@
	$(AT)cp -f $(@:.o=.d) $(@:.o=.d.tmp)
	$(AT)sed -e 's/.*://' -e 's/\\$$//' < $(@:.o=.d.tmp) | fmt -1 | \
	sed -e 's/^ *//' -e 's/$$/: /' >> $(@:.o=.d)
	$(AT)touch -r $@ $(@:.o=.d)
	$(AT)rm -f $(@:.o=.d.tmp)

%.o : %.S

$(OBJDIR)/static/%.o: %.S Makefile ../common.mk $(OBJDIR)/static/%.d | $(OBJDIR)/static
	@echo "Assembling $<"
	$(AT)$(CC) $(CFLAGS_STATIC) -MMD -c $< -o $@
	$(AT)cp -f $(@:.o=.d) $(@:.o=.d.tmp)
	$(AT)sed -e 's/.*://' -e 's/\\$$//' < $(@:.o=.d.tmp) | fmt -1 | \
	sed -e 's/^ *//' -e 's/$$/: /' >> $(@:.o=.d)
	$(AT)touch -r $@ $(@:.o=.d)
	$(AT)rm -f $(@:.o=.d.tmp)

$(OBJDIR)/shared/%.o: %.S Makefile ../common.mk $(OBJDIR)/shared/%.d | $(OBJDIR)/shared
	@echo "Assembling $<"
	$(AT)$(CC) $(CFLAGS_SHARED) -MMD -c $< -o $@
	$(AT)cp -f $(@:.o=.d) $(@:.o=.d.tmp)
	$(AT)sed -e 's/.*://' -e 's/\\$$//' < $(@:.o=.d.tmp) | fmt -1 | \
	sed -e 's/^ *//' -e 's/$$/: /' >> $(@:.o=.d)
	$(AT)touch -r $@ $(@:.o=.d)
	$(AT)rm -f $(@:.o=.d.tmp)

#
# Auto-generated config file containing options that
# need to be enabled for the app if enabled in the runtime
#
# We always attempt to re-generate this file
# We don't change it all the time as this messes up dependence
# checking for applications.
OPTIONS_FILE_UPTODATE := no
ifneq ("$(wildcard $(OCR_INSTALL)/include/ocr-options_$(OCR_TYPE).h)", "")
  ifeq ($(shell cat $(OCR_INSTALL)/include/ocr-options_$(OCR_TYPE).h | sed '4s/.*RT CFLAGS:\(.*\)\*\//\1/; 4!d' | xargs ), $(shell echo "$(CFLAGS)" | xargs))
    OPTIONS_FILE_UPTODATE := yes
  endif
endif

# We do something similar with configuration options but actually dump them in a build file
# so that we can build without installing and not have to rebuild when we install
OPTIONS_UPTODATE := no
ifneq ("$(wildcard $(OCR_BUILD)/cflags)", "")
  ifeq ($(shell cat $(OCR_BUILD)/cflags | xargs), $(shell echo "$(CFLAGS)" | xargs))
    OPTIONS_UPTODATE := yes
  endif
endif

ifeq ($(OPTIONS_UPTODATE), no)
  # If the options have changed, we make it so that the configuration file
  # looks changed so that things get properly rebuilt. This could be, for
  # example, if the CFLAGS were changed on the command line
  $(shell touch $(OCR_BUILD)/ocr-config.h)
  $(shell rm -f $(OCR_BUILD)/cflags)
  $(shell echo "$(CFLAGS)" | xargs > $(OCR_BUILD)/cflags)
endif
ifeq ($(OPTIONS_FILE_UPTODATE), no)
.PHONY: $(OCR_INSTALL)/include/ocr-options_$(OCR_TYPE).h
$(OCR_INSTALL)/include/ocr-options_$(OCR_TYPE).h: | $(OCR_INSTALL)/include
	@echo "Generating OCR build option file: $@"
	$(AT)$(shell echo "" > $@)
	$(AT)$(shell echo "#ifndef __OCR_OPTIONS_"$(subst -,_,$(OCR_TYPE))"_H__" >> $@)
	$(AT)$(shell echo "#define __OCR_OPTIONS_"$(subst -,_,$(OCR_TYPE))"_H__" >> $@)
	$(AT)$(shell echo "/* Generated based on RT CFLAGS: $(CFLAGS) */" >> $@)
ifneq (,$(findstring -DOCR_ENABLE_EDT_NAMING, $(CFLAGS)))
	$(AT)$(shell echo "#ifndef OCR_ENABLE_EDT_NAMING" >> $@)
	$(AT)$(shell echo "#define OCR_ENABLE_EDT_NAMING" >> $@)
	$(AT)$(shell echo "#endif" >> $@)
endif
ifneq (,$(findstring -DOCR_ENABLE_128_BIT_GUID, $(CFLAGS)))
	$(AT)$(shell echo "#ifndef OCR_ENABLE_128_BIT_GUID" >> $@)
	$(AT)$(shell echo "#define OCR_ENABLE_128_BIT_GUID" >> $@)
	$(AT)$(shell echo "#endif" >> $@)
endif
ifneq (,$(findstring -DOCR_ASSERT, $(CFLAGS)))
	$(AT)$(shell echo "#ifndef OCR_ASSERT" >> $@)
	$(AT)$(shell echo "#define OCR_ASSERT" >> $@)
	$(AT)$(shell echo "#endif" >> $@)
endif
	$(AT)$(shell echo "#endif /* __OCR_OPTIONS_"$(subst -,_,$(OCR_TYPE))"_H__ */" >> $@)
else
$(OCR_INSTALL)/include/ocr-options_$(OCR_TYPE).h: | $(OCR_INSTALL)/include
	;
endif
#
# Include auto-generated dependence files
# We only include the ones for the .o that we need to generate
#
ifeq (${SUPPORTS_STATIC}, yes)
  $(OBJDIR)/static/%.d: ;
  .PRECIOUS: $(OBJDIR)/static/%.d
  -include $(OBJS_STATIC:.o=.d)
endif
ifeq (${SUPPORTS_SHARED}, yes)
  $(OBJDIR)/shared/%.d: ;
  .PRECIOUS: $(OBJDIR)/shared/%.d
  -include $(OBJS_SHARED:.o=.d)
endif
ifeq (${SUPPORTS_EXEC}, yes)
  $(OBJDIR)/exec/%.d: ;
  .PRECIOUS: $(OBJDIR)/exec/%.d
  -include $(OBJS_EXEC:.o=.d)
endif

# Install
INSTALL_TARGETS :=
INSTALL_LIBS    :=
INSTALL_EXES    := $(OCRRUNNER)
ifeq (${SUPPORTS_STATIC}, yes)
  INSTALL_TARGETS += static
  INSTALL_LIBS += $(OCRSTATIC)
endif
ifeq (${SUPPORTS_SHARED}, yes)
  INSTALL_TARGETS += shared
  INSTALL_LIBS += $(OCRSHARED)
endif
ifeq (${SUPPORTS_EXEC}, yes)
  INSTALL_TARGETS += exec
  INSTALL_EXES += $(OCREXEC)
endif

INC_FILES         := $(addprefix extensions/, $(notdir $(wildcard $(OCR_ROOT)/inc/extensions/*.h))) \
                     $(notdir $(wildcard $(OCR_ROOT)/inc/*.h))


# WARNING: This next line actually generates the configurations. This will be cleaned
# up in a later commit.
GENERATE_CONFIGS  := $(shell if [ -d $(OCR_ROOT)/machine-configs/$(OCR_TYPE) ]; then cd $(OCR_ROOT)/machine-configs/$(OCR_TYPE) && ./generate-cfg.sh ; fi)
MACHINE_CONFIGS   := $(notdir $(wildcard $(OCR_ROOT)/machine-configs/$(OCR_TYPE)/*.cfg))

# Install scripts that are potentially needed
SCRIPT_FILES      := Configs/config-generator.py

ifneq (,$(findstring $(OCR_TYPE),"tg-ce tg-xe builder-xe builder-ce"))
  SCRIPT_FILES      += $(addprefix Configs/, ce_config_fix.py combine-configs.py xe_config_fix.py tg-fsim_config_fix.py)
  SCRIPT_FILES      += $(patsubst $(OCR_ROOT)/scripts/%,%,$(wildcard $(OCR_ROOT)/scripts/Blob/*))
endif
ifeq (x86-phi,$(findstring $(OCR_TYPE),x86-phi))
  SCRIPT_FILES      += Configs/combine-configs.py
  DEFAULT_CONFIG    := knl_mcdram.cfg
endif

INSTALLED_LIBS    := $(addprefix $(OCR_INSTALL)/lib/, $(notdir $(INSTALL_LIBS)))
BASE_LIBS         := $(firstword $(dir $(INSTALL_LIBS)))
INSTALLED_EXES    := $(addprefix $(OCR_INSTALL)/bin/, $(notdir $(INSTALL_EXES)))
BASE_EXES         := $(firstword $(dir $(INSTALL_EXES)))
INSTALLED_INCS    := $(addprefix $(OCR_INSTALL)/include/, $(INC_FILES)) $(OCR_INSTALL)/include/ocr-options_$(OCR_TYPE).h
INSTALLED_CONFIGS := $(addprefix $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE)/, $(MACHINE_CONFIGS))
INSTALLED_SCRIPTS := $(addprefix $(OCR_INSTALL)/share/ocr/scripts/, $(SCRIPT_FILES))

# Special include files for tg-ce and tg-xe
# This is a bit ugly but putting this directly in the TG makefiles
# means changing a lot of things. This is required for Jenkins
# as the build directory is no longer available when the app
# is building but TGKRNL still needs tg-bin-files.h
ifeq ($(OCR_TYPE), tg-xe)
  INSTALLED_INCS    += $(OCR_INSTALL)/include/tg-bin-files.h
  $(OCR_INSTALL)/include/tg-bin-files.h: $(OCR_BUILD)/tg-bin-files.h
	$(AT)$(RM) -f $(OCR_INSTALL)/include/tg-bin-files.h
	$(AT)$(CP) $(OCR_BUILD)/tg-bin-files.h $(OCR_INSTALL)/include/
endif

ifeq ($(OCR_TYPE), tg-ce)
  INSTALLED_INCS    += $(OCR_INSTALL)/include/tg-bin-files.h
  $(OCR_INSTALL)/include/tg-bin-files.h: $(OCR_BUILD)/tg-bin-files.h
	$(AT)$(RM) -f $(OCR_INSTALL)/include/tg-bin-files.h
	$(AT)$(CP) $(OCR_BUILD)/tg-bin-files.h $(OCR_INSTALL)/include/
endif

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)

  $(OCR_INSTALL)/lib/%: $(BASE_LIBS)% | $(OCR_INSTALL)/lib grablock
	$(AT)$(RM) -f $@
	$(AT)install -m 0644 $< $@

  $(OCR_INSTALL)/bin/%: $(BASE_EXES)% | $(OCR_INSTALL)/bin grablock
	$(AT)$(RM) -f $@
	$(AT)install -m 0755 $< $@

  $(OCR_INSTALL)/include/%: $(OCR_ROOT)/inc/% | $(OCR_INSTALL)/include $(OCR_INSTALL)/include/extensions grablock
	$(AT)$(RM) -f $@
	$(AT)install -m 0644 $< $@

  $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE)/%: $(OCR_ROOT)/machine-configs/$(OCR_TYPE)/% | $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE) grablock
	$(AT)$(RM) -f $@
	$(AT)install -m 0644 $< $@

  $(OCR_INSTALL)/share/ocr/scripts/%: $(OCR_ROOT)/scripts/% | $(OCR_INSTALL)/share/ocr/scripts $(OCR_INSTALL)/share/ocr/scripts/Configs grablock
	$(AT)$(RM) -f $@
	$(AT)install -m 0755 $< $@

  .PHONY: $(OCR_INSTALL)/lib $(OCR_INSTALL)/bin $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE) \
  $(OCR_INSTALL)/include $(OCR_INSTALL)/include/extensions $(OCR_INSTALL)/share/ocr/scripts\
  $(OCR_INSTALL)/share/ocr/scripts/Configs

  $(OCR_INSTALL)/lib $(OCR_INSTALL)/bin $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE) \
  $(OCR_INSTALL)/include $(OCR_INSTALL)/include/extensions $(OCR_INSTALL)/share/ocr/scripts\
  $(OCR_INSTALL)/share/ocr/scripts/Configs:
	$(AT)$(MKDIR) -p $@

else

  $(OCR_INSTALL)/lib/%: $(BASE_LIBS)% | grablock
	$(AT)install -D -m 0644 $< $@

  $(OCR_INSTALL)/bin/%: $(BASE_EXES)% | grablock
	$(AT)install -D -m 0755 $< $@

  $(OCR_INSTALL)/include/%: $(OCR_ROOT)/inc/% | grablock
	$(AT)install -D -m 0644 $< $@

  $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE)/%: $(OCR_ROOT)/machine-configs/$(OCR_TYPE)/% | grablock
	$(AT)install -D -m 0644 $< $@

  $(OCR_INSTALL)/share/ocr/scripts/%: $(OCR_ROOT)/scripts/% | grablock
	$(AT)install -D -m 0755 $< $@

  # Need this for the auto-generated .h file
  .PHONY: $(OCR_INSTALL)/include
  $(OCR_INSTALL)/include:
	$(AT)$(MKDIR) -p $@

endif # Darwin ifeq

.PHONY: grablock
grablock: /tmp/$(subst /,_,$(OCR_INSTALL))_lock

# List the lock file as intermediate so it is removed if things crash
.INTERMEDIATE: /tmp/$(subst /,_,$(OCR_INSTALL))_lock
/tmp/$(subst /,_,$(OCR_INSTALL))_lock:
	@printf "\033[32m Grabbing install lock\033[0m\n"
	$(AT)lockfile "/tmp/$(subst /,_,$(OCR_INSTALL))_lock";

.PHONY: install
install: ${INSTALL_TARGETS} ${INSTALLED_LIBS} ${INSTALLED_EXES} ${INSTALLED_INCS} \
	${INSTALLED_CONFIGS} ${INSTALLED_SCRIPTS}
	@printf "\033[32m Installed OCR for $(OCR_TYPE) into '$(OCR_INSTALL)'\033[0m\n"
	$(AT)if [ -d $(OCR_ROOT)/machine-configs/$(OCR_TYPE) ]; then \
		$(RM) -f $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE)/default.cfg; \
		$(LN) -sf ./$(DEFAULT_CONFIG) $(OCR_INSTALL)/share/ocr/config/$(OCR_TYPE)/default.cfg; \
	fi
	@printf "\033[32m Released lock\033[0m\n"
	$(AT)rm -f "/tmp/$(subst /,_,$(OCR_INSTALL))_lock";

.PHONY: uninstall
uninstall:
	-$(AT)$(RM) $(RMFLAGS) $(INSTALLED_LIBS) $(INSTALLED_EXES) $(INSTALLED_CONFIGS)

.PHONY:clean
clean:
	-$(AT)$(RM) $(RMFLAGS) $(OBJDIR)/* $(OCRSHARED) $(OCRSTATIC) $(OCREXEC) src/*

.PHONY: squeaky
squeaky: clean uninstall
	-$(AT)$(RM) $(RMFLAGS) $(INSTALLED_INCS) $(INSTALLED_SCRIPTS)
