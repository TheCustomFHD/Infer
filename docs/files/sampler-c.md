# `src/sampler.c` — token selection

**~225 lines.** Logit post-processing: repetition penalty, temperature,
top-k, top-p, and the RNG.

## Order of operations

1. **Repetition penalty** over a ring buffer of the last `repeat_last_n`
   accepted tokens. Positive logits are divided, negative multiplied —
   matching llama.cpp.
2. **Greedy shortcut**: `temperature <= 0` returns the argmax directly,
   skipping everything below. This is what `-t 0` uses, and it makes runs
   deterministic and comparable.
3. **Top-k** by partial selection — repeatedly extract the max into the
   front. For small `k` that beats a full sort of a 248,320-entry vocab.
   When `k >= n_vocab` it falls back to a quicksort/insertion hybrid.
4. **Softmax** over the survivors.
5. **Top-p**: accumulate until the mass reaches `top_p`, truncate,
   renormalise.
6. **Sample** from the remaining distribution.

## RNG

A self-contained 32-bit xorshift, so behaviour is identical on every
platform and nothing depends on the quality of libc `rand()`.

```c
s->rng = p->seed ? (p->seed & 0xFFFFFFFFUL) : 2463534242UL;
```

The mask matters: on i486 `unsigned long` is exactly 32 bits and C89 has
no wider type. An earlier version used a 64-bit seed constant, which
overflowed — caught only when the 32-bit build was first attempted.

## Defaults

`temperature 0.7`, `top_p 0.8`, `top_k 40`, `repeat_penalty 1.05`,
`repeat_last_n 64`, `seed 0` (meaning derive from the clock).

## See also

- [../USER-GUIDE.md](../../USER-GUIDE.md)
