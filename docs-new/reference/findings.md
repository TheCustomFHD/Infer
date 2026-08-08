# Findings index

61 measured findings. Each is a thing that was surprising, cost real
debugging time, or would bite the next person — and each is backed by
a measurement.

Full text: [`../../docs/FINDINGS.md`](../../docs/FINDINGS.md).
The most actionable ones are distilled by symptom in
[`../06-PITFALLS.md`](../06-PITFALLS.md).

## Threading

- **52.** `static` scratch + a thread pool = silent corruption, and four tests that all missed it
- **54.** The thread pool spent more time sleeping than the kernels spent working
- **55.** The same barrier, two platforms, one `while` and one `if`

## Kernels, numerics, optimisation

- **24.** A cache that could only ever hit
- **28.** Three optimisations that measurement rejected
- **29.** Two lane budgets that were never checked against the loop
- **31.** The same idea, opposite verdicts on two backends
- **32.** A lookup table that stopped paying, and then blocked vectorisation
- **33.** A static backend priority list is wrong once the compiler vectorises
- **35.** Which loop shape is fastest depends on the compiler flags
- **38.** A per-format benchmark, and the limits of a synthetic one
- **44.** Raising the ISA baseline changed the output, and FMA was why
- **47.** VPMADDUBSW: the instruction the vectoriser will not find
- **49.** Streaming was no slower than cache-resident, so the limit was uops, not bandwidth
- **51.** MMX Q6_K: two passes over the same bytes, and the MMX floor
- **53.** The scale decode cost more than the arithmetic it fed

## Portability, byte order, alignment

- **4.** The MMX alignment bug — invisible on a dev machine
- **15.** Windows 95 is closer than expected
- **18.** The endianness bug was a type-width bug
- **19.** Byte-order neutrality was nearly free, for a 486 reason
- **20.** Byte-order autodetection is compiler-specific, and guessing is worse than failing
- **23.** A graphics instruction made exact
- **25.** Two compilers, two type systems, and a test that never ran
- **26.** `CC ?= cc` cannot override make's built-in default
- **27.** A byte-order bug that only a big-endian machine can see
- **34.** The i486 baseline is real; the binary still needs a Pentium II

## Interface correctness

- **16.** A tool-call parser that "succeeds" with no arguments
- **17.** Silently ignored options are a correctness bug, not a UX one

## CI and release engineering

- **37.** Two logs, six releases apart, read as a backend comparison
- **39.** `universe` is not enabled on GitHub's runners
- **40.** Two CI faults: a Recommends, and a second workflow
- **41.** Installing a cross-toolchain can break a target that already built
- **42.** `apt-get install` is atomic, and `|| true` on it hides everything
- **43.** A probe must be at least as hard as the build it gates
- **45.** A probe with a hardcoded list is a probe with a blind spot
- **46.** The ISA ladder on real Windows: 1.37×, not the predicted 1.80×

## The model and its quirks

- **1.** Qwen3.5 is not a transformer
- **2.** "Q4_K_M" is not a Q4_K file
- **3.** Q4_0 is the same size as Q4_K, and worse
- **11.** Byte-level BPE breaks UTF-8 in streams
- **12.** Two Jinja details that silently corrupt prompts
- **13.** Small models are unreliable at tool use
- **14.** `\p{N}` matches one digit at a time
- **21.** A prompt cache that can never hit, because BPE re-merges
- **22.** The cache still cannot help stock-template chat

## The Geode, and small-machine cost models

- **5.** MMX registers alias the x87 stack — twice over
- **6.** The Geode's fast L1 window is 4 KB, not 64 KB
- **7.** The Geode is compute-bound, not memory-bound
- **8.** `IMUL` is ~16 cycles and is 65% of the `i8` backend
- **9.** The Geode *does* have branch prediction
- **10.** Microbenchmarks lie about this machine, systematically
- **36.** Instruction count is not the i486's cost model either

## Design decisions

- **30.** us/call is the wrong unit when call sizes differ 6x *(Q6_K subsection retracted — see 61)*
- **48.** 6.3 -> 14.4 tok/s: the kernel was never the bottleneck
- **50.** An ISA ladder was the wrong shape; a gated object is the right one

## SPARC and Solaris (2026 investigation)

- **56.** SPARC: MADV_SEQUENTIAL means something different on Solaris
- **57.** SPARC has no integer-to-float register move, and -O3 makes it worse
- **58.** Eight function calls per super-block, for a single array read
- **59.** The VIS Q6_K kernel is still not worth enabling *(RETRACTED — see 61)*
- **60.** The SPARC wins do not transfer to x86, and the reason is the call
- **61.** Instruction counts cannot arbitrate between two functional units
