# `src/qwen35.c` — the inference engine

**~825 lines.** Implements the `qwen35` architecture. This is the file
that had to be written from the model's actual structure rather than
from a generic transformer template.

## Qwen3.5 is a hybrid, not a plain transformer

Of its 24 blocks:

* blocks **3, 7, 11, 15, 19, 23** — `(il+1) % 4 == 0` — are **gated
  grouped-query attention**
* the other **18** are **Gated DeltaNet** linear-attention layers with a
  recurrent matrix state and no KV cache

A conventional llama.cpp-style decode loop cannot run this model. The
architecture was confirmed against `general.architecture = "qwen35"` in
the GGUF and cross-checked with llama.cpp's reference graph before a line
was written.

## Per block

```
h = x + Mix(RMSNorm(x))          Mix = attention or delta-net
y = h + FFN(RMSNorm_post(h))     FFN = SwiGLU
```

## Attention layers

8 query heads, 2 KV heads (GQA), head_dim 256.

`attn_q` produces **2 × head_dim × n_head**: query and gate interleaved
per head. Q and K get per-head RMSNorm, then interleaved multimodal RoPE
(sections 11/11/10/0, base 1e7 — for text-only input all three axes carry
the same position, so it reduces to NeoX-style RoPE over the first
`n_rot` dimensions). The attention output is multiplied by
`sigmoid(gate)` before `attn_output`.

## Gated DeltaNet layers

16 K heads, 16 V heads, head dim 128.

```
qkv  = attn_qkv @ x            -> [q | k | v]
conv = SiLU(causal_depthwise_conv(qkv, width 4))
q, k = L2-normalise per head
beta = sigmoid(ssm_beta @ x)
g    = ssm_a * softplus(ssm_alpha @ x + ssm_dt_bias)

per head:
    S    *= exp(g)
    S    += k ⊗ (v - Sᵀk) * beta
    out   = Sᵀq / sqrt(head_dim)

out = RMSNorm(out, ssm_norm) * SiLU(z)  ->  ssm_out
```

`S` is stored **transposed** — `S[j*hd + i] == state[i][j]` — so row `j`
is contiguous and every inner loop is a unit-stride dot product.

Cost per token is constant regardless of conversation length, which is
why only the six attention layers grow with context.

## The recurrence is fused

Steps 1+2 (decay, then delta) and 3+4 (update, then readout) are fused
into two passes over `S` instead of four. `S` is 64 KB per head — exactly
the Geode's L1 — so this looked like a large win.

**It measured 1.002x on real hardware.** The Geode's *fast* L1 way is
only 4 KB, so 94% of `S` was already paying a penalty regardless of pass
count. The change is kept (bit-identical output, fewer instructions) but
it is a good example of why the target machine has the final word. See
[../FINDINGS.md](../FINDINGS.md).

## Context memory

* KV cache: `n_ctx × n_head_kv × head_dim` floats **per attention layer**
  (6 of them)
* Recurrent state: `n_v_heads × 128 × 128` floats per delta-net layer
  (18 of them) — this dominates and is independent of `n_ctx`

## Verification

The whole forward pass was validated against an independent NumPy
implementation (`tools/ref_numpy.py`) written from the reference graph,
not from this code. Both produce identical ranked logits.

## See also

- [../ARCHITECTURE.md](../ARCHITECTURE.md) · [backend-c.md](backend-c.md)
