"""Independent NumPy reference implementation of the qwen35 forward pass.

Used only to validate src/qwen35.c during development -- it is not part
of the build and infer never needs Python at runtime.

    python3 tools/ref_numpy.py model.gguf "prompt"
"""

import sys
import struct
import numpy as np

# ---------------------------------------------------------------- GGUF

class GGUF:
    def __init__(self, path):
        self.d = np.memmap(path, dtype=np.uint8, mode='r')
        self.b = self.d.tobytes()[:64 * 1024 * 1024]
        self.off = 4
        rd = self._rd
        ver = rd('I'); ntens = rd('Q'); nkv = rd('Q')
        self.kv = {}
        for _ in range(nkv):
            k = self._str(); t = rd('I')
            self.kv[k] = self._val(t)
        self.tensors = {}
        for _ in range(ntens):
            nm = self._str(); nd = rd('I')
            dims = [rd('Q') for _ in range(nd)]
            tt = rd('I'); o = rd('Q')
            self.tensors[nm] = (dims, tt, o)
        align = self.kv.get('general.alignment', 32)
        self.data_start = (self.off + align - 1) // align * align

    def _rd(self, f):
        s = struct.calcsize(f)
        v = struct.unpack_from('<' + f, self.b, self.off)[0]
        self.off += s
        return v

    def _str(self):
        n = self._rd('Q')
        s = self.b[self.off:self.off + n].decode('utf8', 'replace')
        self.off += n
        return s

    T = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'I', 5: 'i', 6: 'f', 7: '?',
         10: 'Q', 11: 'q', 12: 'd'}

    def _val(self, t):
        if t == 8:
            return self._str()
        if t == 9:
            et = self._rd('I'); n = self._rd('Q')
            return [self._val(et) for _ in range(n)]
        return self._rd(self.T[t])

    def raw(self, name):
        dims, tt, o = self.tensors[name]
        n = 1
        for x in dims:
            n *= x
        sz = {0: n * 4, 1: n * 2, 8: n // 32 * 34, 12: n // 256 * 144,
              13: n // 256 * 176, 14: n // 256 * 210}[tt]
        start = self.data_start + o
        return dims, tt, self.d[start:start + sz]


# ------------------------------------------------------- dequantisation

def f16(b):
    return np.frombuffer(b.tobytes(), dtype='<f2').astype(np.float32)


def deq(tt, buf, nelem):
    if tt == 0:
        return np.frombuffer(buf.tobytes(), dtype='<f4').astype(np.float32)
    if tt == 8:                                     # Q8_0
        b = np.asarray(buf).reshape(-1, 34)
        d = f16(b[:, :2].copy()).reshape(-1, 1)
        q = np.frombuffer(b[:, 2:].copy().tobytes(), dtype=np.int8)
        return (d * q.reshape(-1, 32).astype(np.float32)).ravel()
    if tt == 12:                                    # Q4_K
        b = np.asarray(buf).reshape(-1, 144)
        d = f16(np.ascontiguousarray(b[:, 0:2])).reshape(-1, 1)
        dmin = f16(np.ascontiguousarray(b[:, 2:4])).reshape(-1, 1)
        sc = b[:, 4:16].astype(np.int32)
        qs = b[:, 16:].astype(np.int32)
        out = np.zeros((b.shape[0], 256), np.float32)
        for j in range(8):
            if j < 4:
                s = sc[:, j] & 63; m = sc[:, j + 4] & 63
            else:
                s = (sc[:, j + 4] & 0xF) | ((sc[:, j - 4] >> 6) << 4)
                m = (sc[:, j + 4] >> 4) | ((sc[:, j] >> 6) << 4)
            blk = qs[:, (j // 2) * 32:(j // 2 + 1) * 32]
            v = (blk & 0xF) if j % 2 == 0 else (blk >> 4)
            out[:, j * 32:(j + 1) * 32] = (d * s.reshape(-1, 1)) * v - dmin * m.reshape(-1, 1)
        return out.ravel()
    if tt == 13:                                    # Q5_K
        b = np.asarray(buf).reshape(-1, 176)
        d = f16(np.ascontiguousarray(b[:, 0:2])).reshape(-1, 1)
        dmin = f16(np.ascontiguousarray(b[:, 2:4])).reshape(-1, 1)
        sc = b[:, 4:16].astype(np.int32)
        qh = b[:, 16:48].astype(np.int32)
        qs = b[:, 48:].astype(np.int32)
        out = np.zeros((b.shape[0], 256), np.float32)
        for j in range(8):
            if j < 4:
                s = sc[:, j] & 63; m = sc[:, j + 4] & 63
            else:
                s = (sc[:, j + 4] & 0xF) | ((sc[:, j - 4] >> 6) << 4)
                m = (sc[:, j + 4] >> 4) | ((sc[:, j] >> 6) << 4)
            blk = qs[:, (j // 2) * 32:(j // 2 + 1) * 32]
            lo = (blk & 0xF) if j % 2 == 0 else (blk >> 4)
            bit = 1 << j
            hi = ((qh & bit) > 0).astype(np.int32) * 16
            out[:, j * 32:(j + 1) * 32] = (d * s.reshape(-1, 1)) * (lo + hi) - dmin * m.reshape(-1, 1)
        return out.ravel()
    if tt == 14:                                    # Q6_K
        b = np.asarray(buf).reshape(-1, 210)
        ql = b[:, 0:128].astype(np.int32)
        qh = b[:, 128:192].astype(np.int32)
        sc = np.frombuffer(np.ascontiguousarray(b[:, 192:208]).tobytes(),
                           dtype=np.int8).reshape(-1, 16).astype(np.int32)
        d = f16(np.ascontiguousarray(b[:, 208:210])).reshape(-1, 1)
        out = np.zeros((b.shape[0], 256), np.float32)
        for n in range(2):
            o = n * 128; qlo = n * 64; qho = n * 32; sco = n * 8
            for l in range(32):
                i = l // 16
                q1 = ((ql[:, qlo + l] & 0xF) | (((qh[:, qho + l] >> 0) & 3) << 4)) - 32
                q2 = ((ql[:, qlo + l + 32] & 0xF) | (((qh[:, qho + l] >> 2) & 3) << 4)) - 32
                q3 = ((ql[:, qlo + l] >> 4) | (((qh[:, qho + l] >> 4) & 3) << 4)) - 32
                q4 = ((ql[:, qlo + l + 32] >> 4) | (((qh[:, qho + l] >> 6) & 3) << 4)) - 32
                out[:, o + l] = (d.ravel() * sc[:, sco + i] * q1)
                out[:, o + l + 32] = (d.ravel() * sc[:, sco + i + 2] * q2)
                out[:, o + l + 64] = (d.ravel() * sc[:, sco + i + 4] * q3)
                out[:, o + l + 96] = (d.ravel() * sc[:, sco + i + 6] * q4)
        return out.ravel()
    raise ValueError("type %d" % tt)


# Small tensors are cached; big matrices are dequantised on demand and
# discarded so the reference fits in a modest amount of RAM.
CACHE = {}
CACHE_MAX = 4 * 1024 * 1024


def T(g, name):
    if name in CACHE:
        return CACHE[name]
    dims, tt, buf = g.raw(name)
    n = 1
    for x in dims:
        n *= x
    v = deq(tt, buf, n)
    # GGUF dims are [ne0, ne1] with ne0 fastest -> rows of length ne0
    v = v.reshape(list(reversed(dims)))
    if v.nbytes <= CACHE_MAX:
        CACHE[name] = v
    return v


# ------------------------------------------------------------ the model

def rms(x, w, eps):
    return x / np.sqrt((x * x).mean() + eps) * w


def silu(x):
    return x / (1.0 + np.exp(-x))


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def softplus(x):
    return np.log1p(np.exp(np.minimum(x, 20.0))) + np.maximum(x - 20.0, 0.0)


def main():
    path = sys.argv[1]
    g = GGUF(path)
    kv = g.kv
    P = 'qwen35.'
    n_layer = kv[P + 'block_count']
    n_embd = kv[P + 'embedding_length']
    n_head = kv[P + 'attention.head_count']
    n_kv = kv[P + 'attention.head_count_kv']
    hd = kv[P + 'attention.key_length']
    n_rot = kv[P + 'rope.dimension_count']
    base = kv[P + 'rope.freq_base']
    eps = kv[P + 'attention.layer_norm_rms_epsilon']
    d_state = kv[P + 'ssm.state_size']
    n_kh = kv[P + 'ssm.group_count']
    n_vh = kv[P + 'ssm.time_step_rank']
    kwid = kv[P + 'ssm.conv_kernel']
    interval = kv[P + 'full_attention_interval']
    key_dim = d_state * n_kh
    val_dim = d_state * n_vh
    conv_dim = key_dim * 2 + val_dim

    toks = [int(x) for x in sys.argv[2].split(',')]
    print("tokens", toks)

    # token_embd is 248k x 1024; dequantise only the rows we need.
    emb_dims, emb_tt, emb_buf = g.raw('token_embd.weight')
    emb_rowsz = len(emb_buf) // emb_dims[1]

    def emb_row(i):
        return deq(emb_tt, emb_buf[i * emb_rowsz:(i + 1) * emb_rowsz], emb_dims[0])

    kcache = {}
    vcache = {}
    conv_st = {}
    ssm_st = {}

    for pos, tok in enumerate(toks):
        x = emb_row(tok).astype(np.float32).copy()
        for il in range(n_layer):
            B = 'blk.%d.' % il
            recr = ((il + 1) % interval) != 0
            xn = rms(x, T(g, B + 'attn_norm.weight'), eps)

            if not recr:
                qf = T(g, B + 'attn_q.weight') @ xn
                k = T(g, B + 'attn_k.weight') @ xn
                v = T(g, B + 'attn_v.weight') @ xn
                qf = qf.reshape(n_head, 2, hd)
                q = qf[:, 0, :].copy()
                gate = qf[:, 1, :].copy()
                qn = T(g, B + 'attn_q_norm.weight')
                kn = T(g, B + 'attn_k_norm.weight')
                q = np.stack([rms(q[h], qn, eps) for h in range(n_head)])
                k = k.reshape(n_kv, hd)
                k = np.stack([rms(k[h], kn, eps) for h in range(n_kv)])

                half = n_rot // 2
                inv = base ** (-np.arange(0, half) * 2.0 / n_rot)
                ang = pos * inv
                cs, sn = np.cos(ang), np.sin(ang)
                for arr in (q, k):
                    a = arr[:, :half].copy()
                    b = arr[:, half:n_rot].copy()
                    arr[:, :half] = a * cs - b * sn
                    arr[:, half:n_rot] = a * sn + b * cs

                kcache.setdefault(il, []).append(k)
                vcache.setdefault(il, []).append(v.reshape(n_kv, hd))
                K = np.stack(kcache[il])           # [t, n_kv, hd]
                V = np.stack(vcache[il])
                gqa = n_head // n_kv
                out = np.zeros((n_head, hd), np.float32)
                for h in range(n_head):
                    kh = h // gqa
                    s = (K[:, kh, :] @ q[h]) / np.sqrt(hd)
                    s = np.exp(s - s.max()); s /= s.sum()
                    out[h] = s @ V[:, kh, :]
                out = out * sigmoid(gate)
                mix = T(g, B + 'attn_output.weight') @ out.ravel()
            else:
                qkv = T(g, B + 'attn_qkv.weight') @ xn
                z = T(g, B + 'attn_gate.weight') @ xn
                beta = sigmoid(T(g, B + 'ssm_beta.weight') @ xn)
                alpha = T(g, B + 'ssm_alpha.weight') @ xn
                aa = T(g, B + 'ssm_a')
                dt = T(g, B + 'ssm_dt.bias')
                gg = aa * softplus(alpha + dt)

                cw = T(g, B + 'ssm_conv1d.weight')   # [conv_dim, kwid]
                hist = conv_st.setdefault(il, np.zeros((conv_dim, kwid - 1), np.float32))
                full = np.concatenate([hist, qkv.reshape(-1, 1)], axis=1)
                conv = (full * cw).sum(axis=1)
                conv_st[il] = full[:, 1:].copy()
                conv = silu(conv)

                q = conv[:key_dim].reshape(n_kh, d_state)
                k = conv[key_dim:2 * key_dim].reshape(n_kh, d_state)
                v = conv[2 * key_dim:].reshape(n_vh, d_state)
                q = q / np.sqrt((q * q).sum(-1, keepdims=True) + eps)
                k = k / np.sqrt((k * k).sum(-1, keepdims=True) + eps)

                S = ssm_st.setdefault(il, np.zeros((n_vh, d_state, d_state), np.float32))
                out = np.zeros((n_vh, d_state), np.float32)
                for h in range(n_vh):
                    kh = h % n_kh
                    S[h] *= np.exp(gg[h])
                    # S[i][j]; delta_j = (v_j - sum_i S[i][j] k_i) * beta
                    delta = (v[h] - S[h].T @ k[kh]) * beta[h]
                    S[h] += np.outer(k[kh], delta)
                    out[h] = (S[h].T @ q[kh]) / np.sqrt(d_state)

                nw = T(g, B + 'ssm_norm.weight')
                out = np.stack([rms(out[h], nw, eps) for h in range(n_vh)])
                out = out * silu(z).reshape(n_vh, d_state)
                mix = T(g, B + 'ssm_out.weight') @ out.ravel()

            x = x + mix
            xn = rms(x, T(g, B + 'post_attention_norm.weight'), eps)
            a = silu(T(g, B + 'ffn_gate.weight') @ xn)
            b = T(g, B + 'ffn_up.weight') @ xn
            x = x + T(g, B + 'ffn_down.weight') @ (a * b)

    xn = rms(x, T(g, 'output_norm.weight'), eps)
    # logits = token_embd @ xn, computed in row chunks to bound memory
    nv = emb_dims[1]
    logits = np.zeros(nv, np.float32)
    CH = 8192
    for i0 in range(0, nv, CH):
        i1 = min(i0 + CH, nv)
        blk = deq(emb_tt, emb_buf[i0 * emb_rowsz:i1 * emb_rowsz],
                  (i1 - i0) * emb_dims[0]).reshape(i1 - i0, emb_dims[0])
        logits[i0:i1] = blk @ xn
    top = np.argsort(-logits)[:10]
    print("top10:", [(int(t), float(logits[t])) for t in top])


main()
