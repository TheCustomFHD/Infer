infer -- prebuilt binaries
===========================

Six binaries. Windows gets a four-step ISA ladder because Windows
users download binaries; Linux users generally build their own.

  infer-<version>-windows.exe          32-bit, i486 floor -- XP, Geode
  infer-<version>-windows-sse2.exe     32-bit, Pentium 4 and later
  infer-<version>-windows64.exe        64-bit, any x64
  infer-<version>-windows64-avx2.exe   64-bit, Haswell (2013) and later

  infer-<version>-linux                32-bit, i486 floor
  infer-<version>-linux64              64-bit


WHICH WINDOWS BUILD?
--------------------

Take the newest your CPU supports:

  windows64-avx2   Core i-series 4xxx / Ryzen and later   fastest
  windows64        any 64-bit x86
  windows-sse2     Pentium 4, Athlon 64 and later
  windows          anything older, incl. 486 + MMX and Geode

These do NOT fall back. An avx2 binary faults with an illegal
instruction on a CPU without AVX2 -- that is the cost of a build
with no runtime check in the hot loop.

Two caveats worth knowing:

  * AVX needs Windows 7 SP1 or later even on capable silicon.
    Windows XP never saved the ymm registers across a context
    switch, so the avx2 build is not an XP build.
  * Only the plain `windows.exe` targets the i486 baseline. It is
    the one to use on a Geode, and the only one we claim runs on
    period hardware.

Measured on one Xeon, 20 tokens, greedy decoding:

  windows        1.61 tok/s
  windows-sse2   2.01 tok/s   1.25x
  windows64      2.27 tok/s   1.41x
  windows64-avx2 2.90 tok/s   1.80x

Your hardware will differ; the ordering should not.


ALL SIX CONTAIN ALL THREE BACKENDS
----------------------------------

  mmx   integer kernels with an MMX inner loop  (~2x faster)
  i8    integer kernels, portable C
  ref   scalar float reference

The fastest one the CPU supports is chosen automatically at startup.
See what yours picks:

    infer-<version>-linux --backend list
    infer-<version>-windows.exe --backend list

Force one with --backend mmx|i8|ref.


ALL FOUR RUN ON A 486
---------------------

Every binary is compiled to an i486 baseline: nothing newer than a 486
is emitted for ordinary code. The MMX kernels are present but gated
behind a runtime CPUID check, so the same file runs on a 486 and
accelerates on a Pentium MMX, K6, Geode LX or anything newer.

There is no separate "MMX build" and "non-MMX build", because one
binary already covers both cases.

Verified in CI on every release: 0 CMOV instructions in our code (CMOV
is a Pentium Pro instruction), the MMX kernels present, and all three
backends producing identical output under greedy decoding.

  Windows caveat, stated plainly: our code is 486-clean, but mingw's C
  runtime uses CMOV inside strtod(), pow() and ldexp(), which infer
  calls. In practice the .exe files are therefore Pentium-Pro-and-later.
  A true 486 running Windows is untested. For a genuine 486, use the
  Linux binary, which is verified at 0 CMOV throughout.

The Windows binaries import exactly 3 DLLs -- KERNEL32, msvcrt, WS2_32
-- all present on a stock Windows XP install, and use no post-XP API.


THE PROFILER VARIANTS
---------------------

About 2% slower. They print nothing extra unless asked, so they are
safe to use as everyday binaries:

    infer-<version>-windows.exe run model.gguf -p "hi" -n 10 \
        --log-perf                      # tokens/sec, seconds/token
    infer-<version>-windows.exe run model.gguf -p "hi" -n 10 \
        --log-perf --log-stages         # + where the time goes
    infer-<version>-windows.exe run model.gguf -p "hi" -n 10 \
        --log-perf --log-stages --log-file perf.txt

All binaries ship with the timers compiled in; they cost nothing
measurable and stay silent unless you pass --log-stages.


CHECKSUMS
---------

SHA256SUMS is included. Verify with:

    sha256sum -c SHA256SUMS


SPARC / SOLARIS
---------------

Not shipped as a binary: building one needs a Solaris toolchain that
cannot be redistributed. Build from source, it is one command:

    gmake solaris                        Sun Studio, 64-bit
    gmake solaris PROFILE=1              ...with per-stage timers
    gmake solaris TOOLCHAIN=gcc          GCC instead of Sun Studio
    gmake solaris NO_VIS=1               leave the VIS kernels out

These are deliberately separate from the x86 targets -- different
compiler, different flags, big-endian, 64-bit, no MMX. The i8 backend
is selected automatically and is 6.5-8.8x faster than ref.

Details, including the off_t/long long trap that makes 32-bit Solaris
builds fail under -Xc, are in COMPILE.md section 6.


BUILDING THE X86 TARGETS YOURSELF
---------------------------------

    make linux
    make windows
    make all

    Flags: PROFILE=1 (stage timers), NATIVE=1 (-march=native, runs
    only on the build machine), BITS=64 (64-bit Linux), and
    NO_MMX=1 / NO_SSE2=1 / NO_AVX2=1 to leave a kernel out.

    make linux PROFILE=1
    make linux BITS=64 NATIVE=1

    make help          the full list
    make test          the unit tests

COMPILE.md gives every command with its expected output. Section 2 is a
literal step-by-step recipe for producing a Windows XP build, including
the static checks that prove it will load on XP.


GETTING A MODEL
---------------

No weights are included. Any Qwen3.5 GGUF works:

    https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF

    infer-<version>-windows.exe run   model.gguf -p "Hello"
    infer-<version>-windows.exe chat  model.gguf
    infer-<version>-windows.exe serve model.gguf

No chat template is bundled either. infer uses the one inside the GGUF
by default. To use a community-maintained one instead:

    https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates

    infer-<version>-windows.exe chat model.gguf --template qwen-fixed.jinja
