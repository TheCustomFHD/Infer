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
VERSION = 1.15.1
SUFFIX  = -$(VERSION)
BIN     = $(TARGET)$(SUFFIX)

.PHONY: vis-shim-test all linux linux-profile windows windows-profile \
        solaris solaris-profile solaris-gcc solaris-gcc-profile \
        solaris-fast solaris-test \
        test bench clean version checkversion help

all: linux linux-profile windows windows-profile

help:
	@echo "make linux              Linux/x86, all backends, i486 baseline"
	@echo "make linux-profile      ...plus per-stage timers (~2% slower)"
	@echo "make windows            Windows XP/x86, all backends, i486 baseline"
	@echo "make windows-profile    ...plus per-stage timers (~2% slower)"
	@echo "make all                all four"
	@echo ""
	@echo "make solaris            Solaris/SPARC, Sun Studio, 64-bit"
	@echo "make solaris-profile    ...plus per-stage timers"
	@echo "make solaris-gcc        Solaris/SPARC, GCC, 64-bit"
	@echo "make vis-shim-test      VIS kernels vs i8 on any host (types/arith only)"
	@echo "make solaris-vis        Solaris/SPARC + VIS kernels (Sun Studio)"
	@echo "make solaris-gcc-vis    Solaris/SPARC + VIS kernels (GCC)"
	@echo ""
	@echo "make test / bench / clean / version / checkversion"

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
           -march=i486 -mtune=geode -mmmx -mno-sse -mno-sse2 -mfpmath=387 \
           -DINFER_HAVE_MMX

linux: $(CORE) $(POSIX) $(MMXSRC) $(HDRS)
	$(CC) $(X86FLAGS) -m32 -D_FILE_OFFSET_BITS=64 \
		-o $(BIN)-linux $(CORE) $(POSIX) $(MMXSRC) $(LDLIBS)

linux-profile: $(CORE) $(POSIX) $(MMXSRC) $(HDRS)
	$(CC) $(X86FLAGS) -m32 -D_FILE_OFFSET_BITS=64 -DINFER_PROFILE \
		-o $(BIN)-linux-profile $(CORE) $(POSIX) $(MMXSRC) $(LDLIBS)

# -D_WIN32_WINNT=0x0501 pins the Windows API surface to XP, so the
# compiler refuses anything newer and the import table stays clean.
windows: $(CORE) $(WIN32) $(MMXSRC) $(HDRS)
	$(WINCC) $(X86FLAGS) -D_WIN32_WINNT=0x0501 \
		-o $(BIN)-windows.exe $(CORE) $(WIN32) $(MMXSRC) -lws2_32

windows-profile: $(CORE) $(WIN32) $(MMXSRC) $(HDRS)
	$(WINCC) $(X86FLAGS) -D_WIN32_WINNT=0x0501 -DINFER_PROFILE \
		-o $(BIN)-windows-profile.exe $(CORE) $(WIN32) $(MMXSRC) -lws2_32

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
SUNTARGET = native
SUNOPT    = -xO5 -xunroll=16
SUNBITS   = -m64
SUNFLAGS  = -Xc -xc99=none $(SUNOPT) $(SUNBITS) -xtarget=$(SUNTARGET) \
            -DINFER_BIG_ENDIAN=1
SOLLIBS   = -lm -lsocket -lnsl

solaris: $(CORE) $(POSIX)
	$(SUNCC) $(SUNFLAGS) -o $(BIN)-solaris $(CORE) $(POSIX) $(SOLLIBS)
	@echo "note: scalar build, i8 backend only."
	@echo "      backend_vis.c is NOT compiled in -- use 'make solaris-vis' for that."

solaris-profile: $(CORE) $(POSIX)
	$(SUNCC) $(SUNFLAGS) -DINFER_PROFILE \
		-o $(BIN)-solaris-profile $(CORE) $(POSIX) $(SOLLIBS)

# GCC on Solaris. -m64 for the same reason; GCC does define long long,
# so _FILE_OFFSET_BITS would work here, but 64-bit makes it moot.
solaris-gcc: $(CORE) $(POSIX)
	$(GNUCC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter -O2 \
		-m64 -DINFER_BIG_ENDIAN=1 \
		-o $(BIN)-solaris $(CORE) $(POSIX) $(SOLLIBS)

solaris-gcc-profile: $(CORE) $(POSIX)
	$(GNUCC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter -O2 \
		-m64 -DINFER_BIG_ENDIAN=1 -DINFER_PROFILE \
		-o $(BIN)-solaris-profile $(CORE) $(POSIX) $(SOLLIBS)

# Opt-in, non-IEEE float. Verify with t_backend before trusting output.
solaris-fast: $(CORE) $(POSIX)
	$(SUNCC) $(SUNFLAGS) -fast \
		-o $(BIN)-solaris $(CORE) $(POSIX) $(SOLLIBS)

# Solaris make has no pattern rules we can rely on, so these are spelled
# out rather than sharing the rules below.
# ---------------------------------------------------------------------
# VIS builds. -xarch=sparcvis -xvis (Sun Studio) / -mcpu=ultrasparc -mvis (GCC)
# select the VIS 1 instruction set, present on every UltraSPARC.
# ---------------------------------------------------------------------
solaris-vis: $(CORE) $(POSIX) $(VISSRC)
	$(SUNCC) $(SUNFLAGS) -xarch=sparcvis -xvis -DINFER_HAVE_VIS \
		-o $(BIN)-solaris $(CORE) $(POSIX) $(VISSRC) $(SOLLIBS)

solaris-vis-profile: $(CORE) $(POSIX) $(VISSRC)
	$(SUNCC) $(SUNFLAGS) -xarch=sparcvis -xvis -DINFER_HAVE_VIS -DINFER_PROFILE \
		-o $(BIN)-solaris-profile $(CORE) $(POSIX) $(VISSRC) $(SOLLIBS)

solaris-gcc-vis: $(CORE) $(POSIX) $(VISSRC)
	$(GNUCC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-m64 -mcpu=ultrasparc -mvis -DINFER_HAVE_VIS -DINFER_BIG_ENDIAN=1 \
		-o $(BIN)-solaris $(CORE) $(POSIX) $(VISSRC) $(SOLLIBS)

solaris-gcc-vis-profile: $(CORE) $(POSIX) $(VISSRC)
	$(GNUCC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-m64 -mcpu=ultrasparc -mvis -DINFER_HAVE_VIS -DINFER_BIG_ENDIAN=1 \
		-DINFER_PROFILE \
		-o $(BIN)-solaris-profile $(CORE) $(POSIX) $(VISSRC) $(SOLLIBS)

solaris-test: $(CORE) $(POSIX)
	@mkdir -p $(BUILD)
	$(SUNCC) $(SUNFLAGS) -o $(BUILD)/t_endian tests/t_endian.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(SOLLIBS)
	$(SUNCC) $(SUNFLAGS) -o $(BUILD)/t_backend tests/t_backend.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(SOLLIBS)
	$(SUNCC) $(SUNFLAGS) -o $(BUILD)/t_tokenizer tests/t_tokenizer.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/tokenizer.c \
		$(SRCDIR)/sys_posix.c $(SOLLIBS)
	@echo "run: ./$(BUILD)/t_endian [model.gguf]"

# VIS assumptions. Run this FIRST on any new SPARC box: it proves the
# exactness trick the kernels depend on, and needs no model.
solaris-gcc-vis-test: tests/t_vis.c
	@mkdir -p $(BUILD)
	$(GNUCC) -std=gnu89 -Wall -Wextra -O2 -m64 -mcpu=ultrasparc -mvis \
		-DINFER_HAVE_VIS -o $(BUILD)/t_vis tests/t_vis.c
	$(GNUCC) -std=gnu89 -Wall -Wextra -O2 -m64 -mcpu=ultrasparc -mvis \
		-DINFER_HAVE_VIS -DINFER_BIG_ENDIAN=1 -I$(SRCDIR) \
		-o $(BUILD)/t_viskern tests/t_viskern.c $(VISSRC) \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/util.c \
		$(SRCDIR)/prof.c $(SRCDIR)/gguf.c $(SRCDIR)/sys_posix.c -lm
	$(GNUCC) -std=gnu89 -Wall -Wextra -O2 -m64 -mcpu=ultrasparc -mvis \
		-DINFER_HAVE_VIS -DINFER_BIG_ENDIAN=1 -I$(SRCDIR) \
		-o $(BUILD)/t_vissat tests/t_vissat.c $(VISSRC) \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/util.c \
		$(SRCDIR)/prof.c $(SRCDIR)/gguf.c $(SRCDIR)/sys_posix.c -lm
	@echo "run: ./$(BUILD)/t_vis   then   ./$(BUILD)/t_viskern   then   ./$(BUILD)/t_vissat"

vis-shim-test: tests/t_viskern.c $(VISSRC)
	@mkdir -p $(BUILD)
	@echo "NOTE: this runs on the HOST's byte order. It checks types and"
	@echo "      arithmetic, NOT lane order. Use solaris-*-vis-test on a"
	@echo "      big-endian machine for that. (Finding 27.)"
	$(CC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-D__SUNPRO_C=0x5150 -Itests/vis_shim \
		-DINFER_HAVE_VIS -I$(SRCDIR) \
		-o $(BUILD)/t_viskern_sun tests/t_viskern.c $(VISSRC) \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/util.c \
		$(SRCDIR)/prof.c $(SRCDIR)/gguf.c $(SRCDIR)/sys_posix.c -lm
	$(CC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-D__builtin_vis_fmul8x16=shim_fmul8x16 \
		-D__builtin_vis_fpadd16=shim_fpadd16 \
		-D__builtin_vis_fpadd32=shim_fpadd32 \
		-D__builtin_vis_fmuld8ulx16=shim_fmuld8ulx16 \
		-include tests/vis_shim/gcc_shim.h \
		-DINFER_HAVE_VIS -I$(SRCDIR) \
		-o $(BUILD)/t_viskern_gcc tests/t_viskern.c $(VISSRC) \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/util.c \
		$(SRCDIR)/prof.c $(SRCDIR)/gguf.c $(SRCDIR)/sys_posix.c -lm
	@echo "run: ./$(BUILD)/t_viskern_sun   then   ./$(BUILD)/t_viskern_gcc"

solaris-vis-test: tests/t_vis.c
	@mkdir -p $(BUILD)
	$(SUNCC) -Xc -xc99=none -xO3 -m64 -xarch=sparcvis -xvis -DINFER_HAVE_VIS \
		-o $(BUILD)/t_vis tests/t_vis.c
	$(SUNCC) $(SUNFLAGS) -xarch=sparcvis -xvis -DINFER_HAVE_VIS -I$(SRCDIR) \
		-o $(BUILD)/t_viskern tests/t_viskern.c $(VISSRC) \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/util.c \
		$(SRCDIR)/prof.c $(SRCDIR)/gguf.c $(SRCDIR)/sys_posix.c $(SOLLIBS)
	$(SUNCC) $(SUNFLAGS) -xarch=sparcvis -xvis -DINFER_HAVE_VIS \
		-o $(BUILD)/t_backend tests/t_backend.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(VISSRC) $(SRCDIR)/util.c $(SRCDIR)/prof.c \
		$(SRCDIR)/sys_posix.c $(SOLLIBS)
	$(SUNCC) $(SUNFLAGS) -xarch=sparcvis -xvis -DINFER_HAVE_VIS -I$(SRCDIR) \
		-o $(BUILD)/t_vissat tests/t_vissat.c $(VISSRC) \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/util.c \
		$(SRCDIR)/prof.c $(SRCDIR)/gguf.c $(SRCDIR)/sys_posix.c $(SOLLIBS)
	@echo "run: ./$(BUILD)/t_vis   then   ./$(BUILD)/t_viskern   then   ./$(BUILD)/t_vissat   then   ./$(BUILD)/t_backend model.gguf"

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
