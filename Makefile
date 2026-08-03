# infer -- GGUF inference server, ANSI C (C89), no dependencies.
#
#   make                 native build
#   make solaris         Solaris / SPARC with Sun Studio (adds -lsocket -lnsl)
#   make solaris-gcc     Solaris / SPARC with GCC
#   make i486            32-bit build tuned for i486 (needs gcc-multilib)
#   make windows         Windows XP build (needs i686-w64-mingw32-gcc)
#   make test            build the unit tests
#   make clean
#
# The only libraries linked are the C runtime, libm and the OS socket
# library. Nothing else is required to build or to run.

CC      ?= cc
CFLAGS  ?= -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter -O2
LDLIBS  ?= -lm

SRCDIR  := src
BUILD   := build

# Platform-independent sources.
CORE := \
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

# Exactly one backend of each kind is linked in. Porting to a new OS
# means writing these two files and nothing else.
POSIX := $(SRCDIR)/net_posix.c $(SRCDIR)/sys_posix.c
WIN32 := $(SRCDIR)/net_win32.c $(SRCDIR)/sys_win32.c

HDRS := $(SRCDIR)/infer.h $(SRCDIR)/prof.h $(SRCDIR)/jinja.h \
        $(SRCDIR)/jinja_priv.h $(SRCDIR)/mcp.h $(SRCDIR)/chat.h \
        $(SRCDIR)/opts.h $(SRCDIR)/agent.h $(SRCDIR)/net.h $(SRCDIR)/sys.h \
        $(SRCDIR)/server.h $(SRCDIR)/json.h $(SRCDIR)/unicode_tbl.h

TARGET  = infer

# Version stamped onto every executable, so several builds can sit in
# one directory without being renamed by hand:
#     infer-1.11.0, infer-1.11.0-i486, infer-1.11.0-geode, ...
#
# Written out literally rather than extracted with $(shell ...): that is
# a GNU make extension and this Makefile has to work with Solaris
# /usr/ccs/bin/make and other POSIX makes too. `make checkversion`
# asserts it still matches src/infer.h, and CI runs it.
#
#     make SUFFIX=      ->  plain `infer`, no version in the name
VERSION = 1.12.0
SUFFIX  = -$(VERSION)
BIN     = $(TARGET)$(SUFFIX)

.PHONY: all clean i486 geode windows windows-mmx test bench \
        profile profile-geode profile-i486 profile-windows \
        solaris solaris-gcc solaris-profile solaris-gcc-profile solaris-test

all: $(BIN)

$(BIN): $(CORE) $(POSIX) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(CORE) $(POSIX) $(LDLIBS)

# ---------------------------------------------------------------------
# Solaris / SPARC, with Sun Studio (suncc) or GCC.
#
# Two things differ from Linux and neither is about the CPU:
#
#   1. Solaris keeps the socket API in libsocket/libnsl, not libc, so
#      -lsocket -lnsl are required. Linux needs neither.
#   2. Sun Studio spells its options differently: -Xc -xc99=none for
#      strict C89, -xO3 for optimisation. GCC's -std=c89 -pedantic -O2
#      are not recognised.
#
# So there is nothing to edit by hand:
#
#     make solaris          Sun Studio, SPARC (auto-tunes to the host)
#     make solaris-gcc      GCC on Solaris
#     make solaris-profile  Sun Studio + stage timers
#
# -xtarget=native tunes for the machine doing the build. Use
# -xtarget=ultra2 / ultra3 to build on one box for another, e.g.
#     make solaris SUNTARGET=ultra2
#
# The engine is portable C89: the i8 backend (integer kernels) is
# selected automatically and is several times faster than ref. There is
# no VIS backend yet; see docs/PORTING.md.
# ---------------------------------------------------------------------
SUNCC      = cc
SUNTARGET  = native
# -DINFER_BIG_ENDIAN=1 is explicit on purpose. Sun Studio does not define
# __BYTE_ORDER__, and under -Xc it suppresses the unprefixed `sparc`, so
# autodetection has less to work with than under GCC. SPARC is always
# big-endian, so state it rather than infer it. (infer.h would now refuse
# to compile rather than guess, but being explicit documents the intent.)
SUNCFLAGS  = -Xc -xc99=none -xO3 -xtarget=$(SUNTARGET) \
             -DINFER_BIG_ENDIAN=1 -D_FILE_OFFSET_BITS=64
SOLLIBS    = -lm -lsocket -lnsl

solaris: $(CORE) $(POSIX)
	$(SUNCC) $(SUNCFLAGS) -o $(BIN) $(CORE) $(POSIX) $(SOLLIBS)

solaris-profile: $(CORE) $(POSIX)
	$(SUNCC) $(SUNCFLAGS) -DINFER_PROFILE \
		-o $(BIN)-profile $(CORE) $(POSIX) $(SOLLIBS)

solaris-gcc: $(CORE) $(POSIX)
	$(CC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter -O2 \
		-DINFER_BIG_ENDIAN=1 -D_FILE_OFFSET_BITS=64 \
		-o $(BIN) $(CORE) $(POSIX) $(SOLLIBS)

solaris-gcc-profile: $(CORE) $(POSIX)
	$(CC) -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter -O2 \
		-DINFER_PROFILE -DINFER_BIG_ENDIAN=1 -D_FILE_OFFSET_BITS=64 \
		-o $(BIN)-profile $(CORE) $(POSIX) $(SOLLIBS)

# Tests, built the same way. Solaris make has no pattern rules we can
# rely on, so these are spelled out.
solaris-test: $(CORE) $(POSIX)
	@mkdir -p $(BUILD)
	$(SUNCC) $(SUNCFLAGS) -o $(BUILD)/t_endian tests/t_endian.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(SOLLIBS)
	$(SUNCC) $(SUNCFLAGS) -o $(BUILD)/t_backend tests/t_backend.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(SOLLIBS)
	$(SUNCC) $(SUNCFLAGS) -o $(BUILD)/t_tokenizer tests/t_tokenizer.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/tokenizer.c \
		$(SRCDIR)/sys_posix.c $(SOLLIBS)
	@echo "run: ./$(BUILD)/t_endian [model.gguf]"

# ---------------------------------------------------------------------
# i486: 32-bit, nothing newer than the 486 instruction set, no SSE/MMX,
# x87 for floating point. Requires 32-bit libc headers
# (Debian/Ubuntu: apt install gcc-multilib libc6-dev-i386).
# ---------------------------------------------------------------------
i486: $(CORE) $(POSIX)
	$(CC) -std=c89 -pedantic -Wall -O2 \
		-m32 -march=i486 -mtune=i486 \
		-mno-sse -mno-sse2 -mno-mmx -mfpmath=387 \
		-D_FILE_OFFSET_BITS=64 \
		-o $(BIN)-i486 $(CORE) $(POSIX) $(LDLIBS)

# ---------------------------------------------------------------------
# Windows XP (32-bit). Only the two backend files change.
# ---------------------------------------------------------------------
# ---------------------------------------------------------------------
# Profiling builds. Identical to the targets above but with
# -DINFER_PROFILE, which compiles in per-stage timers. Without that
# define the timer macros expand to nothing, so the normal builds carry
# no overhead whatsoever.
#
# Run with --log-file perf.txt to capture the report.
# ---------------------------------------------------------------------
profile: $(CORE) $(POSIX)
	$(CC) $(CFLAGS) -DINFER_PROFILE \
		-o $(BIN)-profile $(CORE) $(POSIX) $(LDLIBS)

profile-i486: $(CORE) $(POSIX)
	$(CC) -std=c89 -pedantic -Wall -O2 \
		-m32 -march=i486 -mtune=i486 \
		-mno-sse -mno-sse2 -mno-mmx -mfpmath=387 \
		-DINFER_PROFILE -D_FILE_OFFSET_BITS=64 \
		-o $(BIN)-i486-profile $(CORE) $(POSIX) $(LDLIBS)

profile-geode: $(CORE) $(POSIX) $(SRCDIR)/backend_mmx.c
	$(CC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-m32 -march=geode -mtune=geode \
		-mmmx -mno-sse -mno-sse2 -mfpmath=387 \
		-DINFER_HAVE_MMX -DINFER_PROFILE -D_FILE_OFFSET_BITS=64 \
		-o $(BIN)-geode-profile $(CORE) $(POSIX) \
		$(SRCDIR)/backend_mmx.c $(LDLIBS)

profile-windows: $(CORE) $(WIN32) $(SRCDIR)/backend_mmx.c
	$(WINCC) -std=gnu89 -Wall -O2 -march=geode -mtune=geode -mmmx \
		-DINFER_HAVE_MMX -DINFER_PROFILE -D_WIN32_WINNT=0x0501 \
		-o $(BIN)-profile.exe $(CORE) $(WIN32) \
		$(SRCDIR)/backend_mmx.c -lws2_32

# ---------------------------------------------------------------------
# Geode LX / Pentium-MMX class: 32-bit, MMX backend compiled in.
# The binary still runs on a 486 if you drop -march (the MMX kernel is
# gated behind a CPUID check at run time), but -march=geode lets GCC use
# the full Geode instruction set for the scalar code too.
# ---------------------------------------------------------------------
geode: $(CORE) $(POSIX) $(SRCDIR)/backend_mmx.c
	$(CC) -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
		-m32 -march=geode -mtune=geode \
		-mmmx -mno-sse -mno-sse2 -mfpmath=387 \
		-DINFER_HAVE_MMX -D_FILE_OFFSET_BITS=64 \
		-o $(BIN)-geode $(CORE) $(POSIX) $(SRCDIR)/backend_mmx.c $(LDLIBS)

WINCC ?= i686-w64-mingw32-gcc

windows: $(CORE) $(WIN32)
	$(WINCC) -std=c89 -Wall -O2 -march=i486 -mtune=i486 \
		-D_WIN32_WINNT=0x0501 \
		-o $(BIN).exe $(CORE) $(WIN32) -lws2_32

# Windows build with the MMX backend (Pentium MMX / Geode and later).
windows-mmx: $(CORE) $(WIN32) $(SRCDIR)/backend_mmx.c
	$(WINCC) -std=gnu89 -Wall -O2 -march=geode -mtune=geode -mmmx \
		-DINFER_HAVE_MMX -D_WIN32_WINNT=0x0501 \
		-o $(BIN)-mmx.exe $(CORE) $(WIN32) $(SRCDIR)/backend_mmx.c -lws2_32

# ---------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------
test: $(BUILD)/t_quant $(BUILD)/t_tokenizer $(BUILD)/t_engine $(BUILD)/t_backend \
      $(BUILD)/t_align $(BUILD)/t_jinja $(BUILD)/t_agent $(BUILD)/t_endian

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/t_quant: tests/t_quant.c $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/t_quant.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c \
	$(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(LDLIBS)

$(BUILD)/t_tokenizer: tests/t_tokenizer.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/t_tokenizer.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c \
	$(SRCDIR)/prof.c $(SRCDIR)/tokenizer.c $(SRCDIR)/sys_posix.c $(LDLIBS)

$(BUILD)/t_engine: tests/t_engine.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/t_engine.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c \
	$(SRCDIR)/prof.c $(SRCDIR)/tokenizer.c $(SRCDIR)/qwen35.c \
		$(SRCDIR)/sys_posix.c $(LDLIBS)

# Cross-mode option behaviour: proves chat, run and serve render the
# same messages to the same prompt, and that the tool-call parser
# handles every shape the model emits (see tests/t_agent.c).
$(BUILD)/t_agent: tests/t_agent.c $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/t_agent.c \
		$(SRCDIR)/agent.c $(SRCDIR)/opts.c $(SRCDIR)/sampler.c \
		$(SRCDIR)/jinja.c $(SRCDIR)/jinja_eval.c \
		$(SRCDIR)/mcp.c $(SRCDIR)/json.c $(SRCDIR)/util.c \
		$(SRCDIR)/net_posix.c $(SRCDIR)/sys_posix.c $(SRCDIR)/tokenizer.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/prof.c $(LDLIBS)

# Byte-order neutrality. Runs on any host; the point is that a
# big-endian build prints exactly the same numbers (see docs/FINDINGS.md).
$(BUILD)/t_endian: tests/t_endian.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/t_endian.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(LDLIBS)

bench: $(BUILD)/t_backend
	@echo "run: ./$(BUILD)/t_backend <model.gguf>"

$(BUILD)/t_backend: tests/t_backend.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/t_backend.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c \
	$(SRCDIR)/prof.c $(SRCDIR)/sys_posix.c $(LDLIBS)

# Alignment guard for the MMX constants. Built with the MMX backend so
# it actually inspects the real constants (see tests/t_align.c).
$(BUILD)/t_align: tests/t_align.c $(SRCDIR)/backend_mmx.c | $(BUILD)
	$(CC) -std=gnu89 -Wall -O2 -mmmx -DINFER_HAVE_MMX \
		-o $@ tests/t_align.c $(SRCDIR)/backend_mmx.c \
		$(SRCDIR)/backend.c $(SRCDIR)/quant.c $(SRCDIR)/gguf.c \
		$(SRCDIR)/util.c $(SRCDIR)/sys_posix.c $(SRCDIR)/prof.c $(LDLIBS)

$(BUILD)/t_jinja: tests/t_jinja.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/t_jinja.c \
		$(SRCDIR)/jinja.c $(SRCDIR)/jinja_eval.c \
		$(SRCDIR)/gguf.c $(SRCDIR)/quant.c $(SRCDIR)/backend.c \
		$(SRCDIR)/util.c $(SRCDIR)/sys_posix.c $(SRCDIR)/prof.c $(LDLIBS)

# Removes this version's outputs, and the unversioned names older
# releases used, so an upgrade does not leave stale binaries behind.
VARIANTS := "" -i486 -geode -profile -i486-profile -geode-profile
clean:
	@for v in $(VARIANTS); do \
		[ "$$v" = '""' ] && v=""; \
		rm -f "$(BIN)$$v" "$(TARGET)$$v"; \
	done
	rm -f $(BIN).exe $(BIN)-mmx.exe $(BIN)-profile.exe
	rm -f $(TARGET).exe $(TARGET)-mmx.exe $(TARGET)-profile.exe
	rm -rf $(BUILD)

# Print the version the build will stamp onto its outputs.
.PHONY: version checkversion
version:
	@echo $(VERSION)

# Guard against VERSION drifting from src/infer.h.
checkversion:
	@h=`sed -n 's/^#define INFER_VERSION "\(.*\)"/\1/p' $(SRCDIR)/infer.h`; \
	if [ "$$h" != "$(VERSION)" ]; then \
		echo "Makefile VERSION=$(VERSION) but infer.h says $$h" >&2; exit 1; \
	fi; echo "version $(VERSION) ok"
