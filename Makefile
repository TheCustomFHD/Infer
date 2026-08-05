# infer -- GGUF inference server, ANSI C (C89), no dependencies.
#
# ---------------------------------------------------------------------
# THE ONLY FOUR TARGETS YOU NORMALLY WANT
# ---------------------------------------------------------------------
#
#   make linux                 Linux / x86        -> infer-1.12.1-linux
#   make linux-profile         ...with profiler   -> infer-1.12.1-linux-profile
#   make windows               Windows XP / x86   -> infer-1.12.1-windows.exe
#   make windows-profile       ...with profiler   -> infer-1.12.1-windows-profile.exe
#
#   make all                   all four of the above
#
# Every one of those contains ALL THREE backends -- mmx, i8, ref -- and
# picks the fastest the CPU supports at run time. Every one is built to
# an i486 baseline, so the same binary runs on a 486 and accelerates on
# anything with MMX. There is no reason to ship more than these.
#
# The -profile variants add per-stage timers, which cost about 2%. They
# print nothing unless you pass --log-stages, so they are safe to use
# as normal binaries, just slightly slower.
#
# ---------------------------------------------------------------------
# SPARC / SOLARIS -- a separate world, see the section further down
# ---------------------------------------------------------------------
#
#   make solaris               Sun Studio, 64-bit  (recommended)
#   make solaris-profile       ...with profiler
#   make solaris-gcc           GCC on Solaris, 64-bit
#   make solaris-gcc-profile   ...with profiler
#
# These are NOT backwards compatible with anything x86. Different
# compiler, different flags, no MMX, big-endian. Kept deliberately
# apart from the x86 targets so neither set constrains the other.
#
# ---------------------------------------------------------------------
# UTILITY
# ---------------------------------------------------------------------
#
#   make test                  build the unit tests into build/
#   make bench                 build just the throughput benchmark
#   make version               print the version
#   make checkversion          assert Makefile matches src/infer.h
#   make clean                 remove every build product
#
# The only libraries linked are the C runtime, libm and the OS socket
# library. Nothing else is required to build or to run.

CC      ?= cc
WINCC   ?= i686-w64-mingw32-gcc
LDLIBS  ?= -lm

SRCDIR  = src
BUILD   = build

# Platform-independent sources.
CORE = \
	$(SRCDIR)/main.c \
	$(SRCDIR)/opts.c \
	$(SRCDIR)/server.c \
	$(SRCDIR)/chat.c \
	$(SRCDIR)/agent.c \
	$(SRCDIR)/jinja.c \
	$(SRCDIR)/jinja_eval.c \
	$(SRCDIR)/mcp.c \
	$(SRCDIR)/qwen35.c \
	$(SRCDIR)/tokenizer.c \
	$(SRCDIR)/sampler.c \
	$(SRCDIR)/quant.c \
	$(SRCDIR)/backend.c \
	$(SRCDIR)/gguf.c \
	$(SRCDIR)/json.c \
	$(SRCDIR)/util.c \
	$(SRCDIR)/prof.c

# Exactly one of each is linked in. Porting to a new OS means writing
# these two files and nothing else.
POSIX = $(SRCDIR)/net_posix.c $(SRCDIR)/sys_posix.c
WIN32 = $(SRCDIR)/net_win32.c $(SRCDIR)/sys_win32.c

# The MMX backend. x86 only, GNU inline asm, hence -std=gnu89.
MMXSRC = $(SRCDIR)/backend_mmx.c

HDRS = $(SRCDIR)/infer.h $(SRCDIR)/prof.h $(SRCDIR)/jinja.h \
       $(SRCDIR)/jinja_priv.h $(SRCDIR)/mcp.h $(SRCDIR)/chat.h \
       $(SRCDIR)/opts.h $(SRCDIR)/agent.h $(SRCDIR)/net.h $(SRCDIR)/sys.h \
       $(SRCDIR)/server.h $(SRCDIR)/json.h $(SRCDIR)/unicode_tbl.h

TARGET  = infer

# Version stamped onto every executable, so several builds can sit in
# one directory without being renamed by hand.
#
# Written out literally rather than extracted with $(shell ...): that is
# a GNU make extension and this Makefile also has to work with Solaris
# /usr/ccs/bin/make. `make checkversion` asserts it still matches
# src/infer.h, and CI runs it.
#
#     make SUFFIX=      ->  plain `infer`, no version in the name
VERSION = 1.17.5
SUFFIX  = -$(VERSION)
BIN     = $(TARGET)$(SUFFIX)

# Per-stage timers. A COMPILE-TIME choice, not a separate target:
#
#     make linux              plain
#     make linux PROFILE=1    same target, timers compiled in
#
# The timers stay compile-time rather than a runtime flag on purpose.
# PROF_STOP sits inside the per-layer path, so a runtime test would put
# a load-and-branch in the hot loop of every build, on machines where
# that is measurable. The binary is named -profile so the two cannot be
# confused.
PROFILE   =
PROFFLAGS = $(PROFILE:1=-DINFER_PROFILE)
PROFSUF   = $(PROFILE:1=-profile)

# ---------------------------------------------------------------------
# Opt-OUT flags. Every accelerated kernel is compiled in by default and
# selected at RUN TIME after a CPUID / feature probe. These switch one
# off at build time, for a smaller binary or to isolate a codegen bug:
#
#     make linux NO_MMX=1
#     make linux NO_AVX2=1 NO_SSE2=1
#     make solaris NO_VIS=1
#
# All-on is the supported configuration. The runtime cost of having
# them is one indirect call per matrix ROW -- measured at the noise
# floor on both x86-64 and an i486-targeted build.
#
# Implemented with indirect variable expansion ($(VAR_$(NO_X))) rather
# than ifeq, because this Makefile must also work with Solaris
# /usr/ccs/bin/make, which has no conditionals.
# ---------------------------------------------------------------------
NO_MMX  =
NO_SSE2 =
NO_AVX2 =
NO_VIS  =

MMXSRC_   = $(SRCDIR)/backend_mmx.c
MMXFLG_   = -DINFER_HAVE_MMX
MMXSRC_1  =
MMXFLG_1  =
SSE2FLG_  = -DINFER_HAVE_SSE2
SSE2FLG_1 =
AVX2FLG_  = -DINFER_HAVE_AVX2
AVX2FLG_1 =
VISSRC_   = $(SRCDIR)/backend_vis.c
VISFLG_   = -DINFER_HAVE_VIS
VISSRC_1  =
VISFLG_1  =

USE_MMXSRC  = $(MMXSRC_$(NO_MMX))
USE_MMXFLG  = $(MMXFLG_$(NO_MMX))
USE_SSE2FLG = $(SSE2FLG_$(NO_SSE2))
USE_AVX2FLG = $(AVX2FLG_$(NO_AVX2))
USE_VISSRC  = $(VISSRC_$(NO_VIS))
USE_VISFLG  = $(VISFLG_$(NO_VIS))

# Linux word size. 32-bit by default: that is what runs on a 486 and on
# the Geode. BITS=64 gives AVX2 sixteen ymm registers instead of eight,
# which is worth having on a modern machine.
#
#     make linux            -> infer-X.Y.Z-linux     (32-bit, 486 floor)
#     make linux BITS=64    -> infer-X.Y.Z-linux64
# NATIVE=1 tunes for the machine doing the build: -march=native
# -mtune=native on GCC/Clang, -xtarget=native on Sun Studio. It REPLACES
# the portable baseline, so the result runs only on that CPU (or one
# with a superset of its features). Never ship a NATIVE build.
#
#     make linux NATIVE=1
#     make solaris NATIVE=1 TOOLCHAIN=gcc
#
# Kernel selection stays a run-time decision either way; NATIVE only
# changes what the compiler may emit for the portable C.
NATIVE       =
NATSUF_      =
NATSUF_1     = -native
NATSUF       = $(NATSUF_$(NATIVE))
SOLNATGCC_   =
SOLNATGCC_1  = -mcpu=native

BITS     = 32
BITSUF_32 =
BITSUF_64 = 64
BITSUF   = $(BITSUF_$(BITS))
# 32-bit needs the LFS macro; on 64-bit off_t is already wide.
LFS_32   = -D_FILE_OFFSET_BITS=64
LFS_64   =
USE_LFS  = $(LFS_$(BITS))
# The i486 baseline only means anything in a 32-bit build.
ARCH_32  = -march=i486 -mtune=geode -mno-sse -mno-sse2 -mfpmath=387
ARCH_64  = -mtune=generic
BASEARCH = $(ARCH_$(BITS))
# NATIVE replaces the baseline outright. Appending -march=native after
# -mno-sse does NOT work: the -mno-* flags are sticky and silently
# leave you with a native-tuned binary that may emit no SIMD at all.
ARCHSEL_  = $(BASEARCH)
ARCHSEL_1 = -march=native -mtune=native
USE_ARCH  = $(ARCHSEL_$(NATIVE))

# Solaris compiler. Sun Studio by default; TOOLCHAIN=gcc switches.
# Deliberately NOT spelled CC: make gives CC a built-in default of
# `cc`, which on Solaris is Studio, so `CC ?= gcc` silently does
# nothing. That cost a release once already (finding 26).
TOOLCHAIN = studio

.PHONY: all linux windows solaris \
        test bench solaris-test vis-shim-test i8-split-test \
        clean version checkversion help

# Everything shippable: portable and 64-bit, each with and without the
# timers. NATIVE=1 is deliberately absent -- it only runs on the machine
# that built it, so it is never a release artifact.
#
#   infer-X.Y.Z-linux                 32-bit, i486 floor, mmx
#   infer-X.Y.Z-linux-profile         ...with stage timers
#   infer-X.Y.Z-linux64               64-bit, i8 autovectorised (SSE2)
#   infer-X.Y.Z-linux64-profile       ...with stage timers
#   infer-X.Y.Z-windows.exe           32-bit, i486 floor, mmx
#   infer-X.Y.Z-windows-profile.exe   ...with stage timers
all:
	@$(MAKE) linux
	@$(MAKE) linux PROFILE=1
	@$(MAKE) linux BITS=64
	@$(MAKE) linux BITS=64 PROFILE=1
	@$(MAKE) windows
	@$(MAKE) windows PROFILE=1

help:
	@echo "TARGETS"
	@echo "  make linux              Linux/x86  -- mmx + i8 + ref"
	@echo "  make windows            Windows/x86 (XP and later), same set"
	@echo "  make solaris            Solaris/SPARC -- vis + i8 + ref"
	@echo "  make all                linux + windows"
	@echo ""
	@echo "Every accelerated kernel is compiled in and picked at RUN TIME"
	@echo "after a CPUID / feature probe. One binary per platform."
	@echo ""
	@echo "FLAGS (combine freely)"
	@echo "  PROFILE=1               per-stage timers, names the binary -profile"
	@echo "  BITS=64                 64-bit Linux build (default 32, runs on a 486)"
	@echo "  TOOLCHAIN=gcc           build Solaris with GCC instead of Sun Studio"
	@echo "  NATIVE=1                -march=native; runs ONLY on this machine"
	@echo "  FAST=1                  Solaris non-IEEE float; verify with t_backend"
	@echo "  NO_MMX=1 NO_SSE2=1      leave a kernel out"
	@echo "  NO_AVX2=1 NO_VIS=1"
	@echo ""
	@echo "  make linux PROFILE=1 BITS=64"
	@echo "  make solaris TOOLCHAIN=gcc NO_VIS=1"
	@echo ""
	@echo "TESTS"
	@echo "  make test               full suite (needs a model for some)"
	@echo "  make i8-split-test      i8 Q4_K exactness, any host, no model"
	@echo "  make vis-shim-test      VIS kernels vs i8 on any host"
	@echo "  make solaris-vis-test   VIS assumptions, on the real machine"


# =====================================================================
#  x86: Linux and Windows
# =====================================================================
#
# One flag set, used by all four x86 targets. The reasoning:
#
#   -std=gnu89       backend_mmx.c uses GNU inline asm. It is the only
#                    file in the project that is not strict C89.
#   -march=i486      BASELINE. Nothing newer than a 486 is emitted for
#                    ordinary code -- in particular no CMOV, which is a
#                    Pentium Pro instruction and faults on a 486.
#   -mtune=geode     SCHEDULING only. Reorders instructions for the
#                    Geode pipeline; never changes the instruction set.
#   -mmmx            lets the compiler use MMX *inside backend_mmx.c*,
#                    which is guarded by a runtime CPUID check.
#   -DINFER_HAVE_MMX compiles that backend in and registers it.
#
# The result: one binary with all three backends, running on a 486 and
# accelerating on anything with MMX. Measured cost of the i486 baseline
# versus letting the compiler use CMOV: 0.6%, in favour of the baseline.

X86FLAGS = -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
           $(USE_ARCH) -mmmx \
           $(USE_MMXFLG) $(USE_SSE2FLG) $(USE_AVX2FLG)

linux: $(CORE) $(POSIX) $(HDRS)
	$(CC) $(X86FLAGS) -m$(BITS) $(USE_LFS) $(PROFFLAGS) \
		-o $(BIN)-linux$(BITSUF)$(NATSUF)$(PROFSUF) \
		$(CORE) $(POSIX) $(USE_MMXSRC) $(LDLIBS)

# -D_WIN32_WINNT=0x0501 pins the Windows API surface to XP, so the
# compiler refuses anything newer and the import table stays clean.
windows: $(CORE) $(WIN32) $(HDRS)
	$(WINCC) $(X86FLAGS) -D_WIN32_WINNT=0x0501 $(PROFFLAGS) \
		-o $(BIN)-windows$(NATSUF)$(PROFSUF).exe \
		$(CORE) $(WIN32) $(USE_MMXSRC) -lws2_32

# =====================================================================
#  SPARC / Solaris
# =====================================================================
#
# Deliberately separate from everything above. Different compiler,
# different option spellings, big-endian, no MMX. Nothing here
# constrains the x86 targets and nothing there constrains these.
#
# THREE things differ from Linux, and none of them is about the CPU:
#
#  1. Solaris keeps the socket API in libsocket/libnsl, not libc.
#     Without -lsocket -lnsl you get undefined socket/bind/gethostbyname
#     at link time. This is the usual reason a hand-rolled GCC build
#     fails on Solaris where Sun Studio succeeds.
#
#  2. Sun Studio spells its options differently: -Xc -xc99=none for
#     strict C90, -xO5 for optimisation, -xtarget= for -march=.
#     GCC's -std=c89 -pedantic -O2 are not recognised by cc at all.
#
#  3. BUILD 64-BIT, AND DO NOT PASS -D_FILE_OFFSET_BITS=64.
#     This is subtle and it broke a real build. In strict C90 mode
#     (-Xc) Sun Studio undefines _LONGLONG_TYPE, because C90 has no
#     `long long`. Solaris's <sys/types.h> then falls back to
#
#         typedef union { double _d; int32_t _l[2]; } longlong_t;
#
#     and with _FILE_OFFSET_BITS=64 on a 32-bit build, off_t becomes
#     that union. You cannot cast to it, assign to it, or do arithmetic
#     on it -- mmap()'s offset argument becomes unusable and the
#     compile fails with "argument #6 is incompatible with prototype".
#
#     In LP64 (-m64) off_t is already a 64-bit long, so large-file
#     support is automatic and the union never appears. Every machine
#     these targets are aimed at -- UltraSPARC IIi and later -- is
#     64-bit, so -m64 is both correct and simpler.
#
# -xO5 and -xunroll=16 come from a user's own flag set on an
# UltraSPARC; the 4-issue in-order pipeline benefits from unrolling and
# neither flag changes results. Override with SUNOPT=.
#
# Deliberately NOT default:
#
#   -fast   A macro that enables -fsimple=2 and -fns, i.e. non-IEEE
#           float: reassociation, flush-to-zero, no gradual underflow.
#           The k-quant scales are fp16 values very close to zero and
#           the DeltaNet recurrence accumulates over 128 steps, so
#           flush-to-zero is a real correctness risk here, not a
#           theoretical one. `make solaris-fast` opts in; check the
#           result with ./build/t_backend model.gguf.
#
#   -xvis   Enables VIS *intrinsics*. There is no VIS code in this
#           project, so it does nothing. Harmless, just pointless.

# VIS backend. Opt-in until measured on real hardware, exactly as the
# MMX backend is on x86. Correctness is never at risk: formats without
# a VIS kernel fall through to the portable i8 path.
VISSRC    = $(SRCDIR)/backend_vis.c

SUNCC     = cc

# The solaris-gcc-* targets must NOT use $(CC): make gives CC a built-in
# default of `cc`, which on Solaris is Sun Studio, so `CC ?= cc` never
# fires and GCC flags get handed to the wrong compiler. Studio then
# reads -std=gnu89 as -s plus td=gnu89 and dies on "-W option with
# unknown program all". Name GCC explicitly. (Finding 26.)
#
# Override if GCC is not on PATH as `gcc`:
#   gmake solaris-gcc-vis GNUCC=/usr/sfw/bin/gcc
GNUCC     = gcc
# -xtarget=generic keeps the binary runnable on any UltraSPARC; this
# used to say `native`, which silently made every Solaris build
# machine-specific. NATIVE=1 opts back in.
SUNTGT_   = generic
SUNTGT_1  = native
SUNTARGET = $(SUNTGT_$(NATIVE))
SUNOPT    = -xO5 -xunroll=16
SUNBITS   = -m64
SUNFLAGS  = -Xc -xc99=none $(SUNOPT) $(SUNBITS) -xtarget=$(SUNTARGET) \
            -DINFER_BIG_ENDIAN=1
SOLLIBS   = -lm -lsocket -lnsl

# One target. TOOLCHAIN picks the compiler, NO_VIS drops the VIS
# kernels, INFER_VIS_Q6K opts the Q6_K kernel back in (it measured no
# faster than the portable path on an UltraSPARC IIi -- finding 30).
#
#     make solaris
#     make solaris TOOLCHAIN=gcc
#     make solaris NO_VIS=1 PROFILE=1
#
# -m64 is not a performance choice: in a 32-bit Solaris build with
# _FILE_OFFSET_BITS=64 and -Xc, off_t becomes a union that cannot be
# passed to mmap(). LP64 makes off_t a plain long. (See the header
# comment further up.)
SOL_CC_studio    = $(SUNCC)
SOL_CC_gcc       = $(GNUCC)
SOL_FLAGS_studio = $(SUNFLAGS) $(SOLVIS_studio$(NO_VIS))
SOL_FLAGS_gcc    = -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
                   -m64 -DINFER_BIG_ENDIAN=1 $(SOLVIS_gcc$(NO_VIS)) \

SOLVIS_studio    = -xarch=sparcvis -xvis
SOLVIS_studio1   =
SOLVIS_gcc       = $(SOLCPU_$(NATIVE)) -mvis
SOLVIS_gcc1      =
SOLCPU_          = -mcpu=ultrasparc
# GCC's -mcpu=native only works when the compiler runs ON a SPARC; a
# cross-compiler rejects it. SOLCPU overrides it for cross builds:
#   gmake solaris TOOLCHAIN=gcc NATIVE=1 SOLCPU_1=-mcpu=ultrasparc3
SOLCPU_1         = -mcpu=native

solaris: $(CORE) $(POSIX) $(HDRS)
	$(SOL_CC_$(TOOLCHAIN)) $(SOL_FLAGS_$(TOOLCHAIN)) \
		$(USE_VISFLG) $(PROFFLAGS) $(USE_FAST) \
		-o $(BIN)-solaris$(NATSUF)$(PROFSUF) \
		$(CORE) $(POSIX) $(USE_VISSRC) $(SOLLIBS)

# Opt-in, non-IEEE float. Verify with t_backend before trusting output.
# Non-IEEE float (-fast: -fsimple=2, flush-to-zero). Opt-in and Studio
# only; verify with t_backend before trusting output. This is a FLAG
# because it is a property of the build, not a different target:
#     gmake solaris FAST=1
# Studio spells it -fast; GCC's nearest equivalent is -Ofast. Both
# relax IEEE semantics, so both are opt-in and both want t_backend run
# before the output is trusted.
FAST            =
FASTFLG_studio  =
FASTFLG_studio1 = -fast
FASTFLG_gcc     =
FASTFLG_gcc1    = -Ofast
USE_FAST = $(FASTFLG_$(TOOLCHAIN)$(FAST))

# ---------------------------------------------------------------------
# SPARC test programs. One target; TOOLCHAIN picks the compiler, exactly
# as `make solaris` does.
#
#     gmake solaris-test                  Sun Studio
#     gmake solaris-test TOOLCHAIN=gcc    GCC
#
# Builds every SPARC-relevant test: the VIS assumptions (t_vis), the
# kernels against i8 (t_viskern), worst-case lane saturation
# (t_vissat), byte order (t_endian) and the backend comparison
# (t_backend, the only one needing a model).
# ---------------------------------------------------------------------
STEST_CC_studio    = $(SUNCC)
STEST_CC_gcc       = $(GNUCC)
STEST_FLAGS_studio = $(SUNFLAGS) $(SOLVIS_studio$(NO_VIS))
STEST_FLAGS_gcc    = -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
                     -m64 -DINFER_BIG_ENDIAN=1 $(SOLVIS_gcc$(NO_VIS))
STEST_CC    = $(STEST_CC_$(TOOLCHAIN))
STEST_FLAGS = $(STEST_FLAGS_$(TOOLCHAIN))
STEST_DEPS  = $(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/util.c \
              $(SRCDIR)/prof.c $(SRCDIR)/gguf.c $(SRCDIR)/sys_posix.c

solaris-test: tests/t_vis.c tests/t_viskern.c tests/t_vissat.c
	@mkdir -p $(BUILD)
	$(STEST_CC) $(STEST_FLAGS) $(USE_VISFLG) \
		-o $(BUILD)/t_vis tests/t_vis.c
	$(STEST_CC) $(STEST_FLAGS) $(USE_VISFLG) -I$(SRCDIR) \
		-o $(BUILD)/t_viskern tests/t_viskern.c $(USE_VISSRC) \
		$(STEST_DEPS) $(SOLLIBS)
	$(STEST_CC) $(STEST_FLAGS) $(USE_VISFLG) -I$(SRCDIR) \
		-o $(BUILD)/t_vissat tests/t_vissat.c $(USE_VISSRC) \
		$(STEST_DEPS) $(SOLLIBS)
	$(STEST_CC) $(STEST_FLAGS) $(USE_VISFLG) -I$(SRCDIR) \
		-o $(BUILD)/t_endian tests/t_endian.c $(USE_VISSRC) \
		$(STEST_DEPS) $(SOLLIBS)
	$(STEST_CC) $(STEST_FLAGS) $(USE_VISFLG) -I$(SRCDIR) \
		-o $(BUILD)/t_backend tests/t_backend.c $(USE_VISSRC) \
		$(STEST_DEPS) $(SOLLIBS)
	@echo "run: ./$(BUILD)/t_vis  ./$(BUILD)/t_viskern  ./$(BUILD)/t_vissat"
	@echo "     ./$(BUILD)/t_endian   then   ./$(BUILD)/t_backend model.gguf"

# Host-side checks. Neither needs a model or a SPARC.
#   vis-shim-test  both VIS compiler branches on any host (finding 27:
#                  little-endian, so it cannot check lane order)
#   i8-split-test  i8 Q4_K split-loop integer exactness (finding 31)
vis-shim-test: tests/t_viskern.c $(VISSRC)
	@mkdir -p $(BUILD)
	@echo "NOTE: runs on the HOST's byte order. Checks types and"
	@echo "      arithmetic, NOT lane order. Use solaris-test on a"
	@echo "      big-endian machine for that. (Finding 27.)"
	$(CC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-D__SUNPRO_C=0x5150 -Itests/vis_shim \
		-DINFER_HAVE_VIS -I$(SRCDIR) \
		-o $(BUILD)/t_viskern_sun tests/t_viskern.c $(VISSRC) \
		$(STEST_DEPS) -lm
	$(CC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-D__builtin_vis_fmul8x16=shim_fmul8x16 \
		-D__builtin_vis_fpadd16=shim_fpadd16 \
		-D__builtin_vis_fpadd32=shim_fpadd32 \
		-D__builtin_vis_fmuld8ulx16=shim_fmuld8ulx16 \
		-include tests/vis_shim/gcc_shim.h \
		-DINFER_HAVE_VIS -I$(SRCDIR) \
		-o $(BUILD)/t_viskern_gcc tests/t_viskern.c $(VISSRC) \
		$(STEST_DEPS) -lm
	@echo "run: ./$(BUILD)/t_viskern_sun   then   ./$(BUILD)/t_viskern_gcc"

i8-split-test: tests/t_i8split.c tests/t_q5split.c tests/t_ident.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu89 -Wall -Wextra -O2 -o $(BUILD)/t_i8split tests/t_i8split.c
	$(CC) -std=gnu89 -Wall -Wextra -O2 -o $(BUILD)/t_q5split tests/t_q5split.c
	$(CC) $(X86FLAGS) -m$(BITS) $(USE_LFS) -I$(SRCDIR) \
		-o $(BUILD)/t_ident tests/t_ident.c \
		$(SRCDIR)/backend.c $(USE_MMXSRC) $(SRCDIR)/quant.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/gguf.c \
		$(SRCDIR)/sys_posix.c $(LDLIBS)
	@echo "run: ./$(BUILD)/t_i8split  ./$(BUILD)/t_q5split  ./$(BUILD)/t_ident"

# =====================================================================
#  Tests
# =====================================================================

TESTFLAGS = -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter -O2

test: $(BUILD)/t_quant $(BUILD)/t_tokenizer $(BUILD)/t_engine \
      $(BUILD)/t_backend $(BUILD)/t_align $(BUILD)/t_jinja \
      $(BUILD)/t_agent $(BUILD)/t_endian $(BUILD)/t_cache

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/t_quant: tests/t_quant.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_quant.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(LDLIBS)

$(BUILD)/t_tokenizer: tests/t_tokenizer.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_tokenizer.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/tokenizer.c \
		$(SRCDIR)/sys_posix.c $(LDLIBS)

$(BUILD)/t_engine: tests/t_engine.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_engine.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/tokenizer.c \
		$(SRCDIR)/qwen35.c $(SRCDIR)/sys_posix.c $(LDLIBS)

$(BUILD)/t_backend: tests/t_backend.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_backend.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(LDLIBS)

# Built with the MMX backend so it inspects the real constants.
$(BUILD)/t_align: tests/t_align.c $(MMXSRC) | $(BUILD)
	$(CC) -std=gnu89 -Wall -O2 -mmmx -DINFER_HAVE_MMX \
		-o $@ tests/t_align.c $(MMXSRC) \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/gguf.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(LDLIBS)

$(BUILD)/t_jinja: tests/t_jinja.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_jinja.c \
		$(SRCDIR)/jinja.c $(SRCDIR)/jinja_eval.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/sys_posix.c $(SRCDIR)/prof.c $(LDLIBS)

# Cross-mode option behaviour: proves chat, run and serve render the
# same messages to the same prompt, and that the tool-call parser
# handles every shape the model emits.
$(BUILD)/t_agent: tests/t_agent.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_agent.c \
		$(SRCDIR)/agent.c $(SRCDIR)/opts.c $(SRCDIR)/sampler.c \
		$(SRCDIR)/jinja.c $(SRCDIR)/jinja_eval.c \
		$(SRCDIR)/mcp.c $(SRCDIR)/json.c $(SRCDIR)/util.c \
		$(SRCDIR)/net_posix.c $(SRCDIR)/sys_posix.c $(SRCDIR)/tokenizer.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/prof.c $(LDLIBS)

# Byte-order neutrality. Runs on any host; the point is that a
# big-endian build prints exactly the same numbers.
$(BUILD)/t_endian: tests/t_endian.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_endian.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(LDLIBS)

# Prompt-prefix reuse must be exactly equivalent to recomputing.
$(BUILD)/t_cache: tests/t_cache.c | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ tests/t_cache.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/tokenizer.c \
		$(SRCDIR)/qwen35.c $(SRCDIR)/sys_posix.c $(LDLIBS)

bench: $(BUILD)/t_backend
	@echo "run: ./$(BUILD)/t_backend <model.gguf>"

# =====================================================================
#  Utility
# =====================================================================

version:
	@echo $(VERSION)

# Guard against VERSION drifting from src/infer.h.
checkversion:
	@h=`sed -n 's/^#define INFER_VERSION "\(.*\)"/\1/p' $(SRCDIR)/infer.h`; \
	if [ "$$h" != "$(VERSION)" ]; then \
		echo "Makefile VERSION=$(VERSION) but infer.h says $$h" >&2; exit 1; \
	fi; echo "version $(VERSION) ok"

# Removes this version's outputs, any other version's outputs, and the
# unversioned names older releases used.
clean:
	@rm -f $(TARGET)-[0-9]*.[0-9]*.[0-9]** 
	@rm -f $(TARGET) $(TARGET).exe $(TARGET)-linux $(TARGET)-windows.exe
	@rm -f $(BIN) $(BIN)-linux $(BIN)-linux-profile
	@rm -f $(BIN)-windows.exe $(BIN)-windows-profile.exe
	@rm -f $(BIN)-solaris $(BIN)-solaris-profile
	@rm -rf $(BUILD)
