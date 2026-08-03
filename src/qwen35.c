/* qwen35.c -- inference engine for the Qwen3.5 ("qwen35") architecture.
 *
 * Qwen3.5 is a HYBRID model. Of its 24 blocks, every 4th (indices 3, 7,
 * 11, 15, 19, 23 -- i.e. (il+1) % full_attention_interval == 0) is a
 * conventional gated grouped-query attention layer with interleaved
 * multimodal RoPE; the other 18 are Gated DeltaNet linear-attention
 * layers carrying a recurrent matrix state instead of a KV cache.
 *
 * Per block:
 *
 *   h  = x + Mix(RMSNorm(x))            Mix = attention or delta-net
 *   y  = h + FFN(RMSNorm_post(h))       FFN = SwiGLU
 *
 * Full attention layer (n_head=8, n_head_kv=2, head_dim=256):
 *   attn_q  -> 2*head_dim*n_head, interleaved [q_head0 | gate_head0 | ...]
 *   q,k are RMS-normed per head, then i-mRoPE'd (sections 11,11,10,0)
 *   out = (softmax(qk/sqrt(d)) v) * sigmoid(gate), then attn_output
 *
 * Gated DeltaNet layer (16 k-heads, 16 v-heads, head dim 128):
 *   attn_qkv -> [q | k | v], causal depthwise conv (kernel 4) + SiLU
 *   q,k L2-normalised; beta = sigmoid(x @ ssm_beta)
 *   g = ssm_a * softplus(x @ ssm_alpha + ssm_dt_bias)
 *   recurrence per head, S is [head_dim x head_dim]:
 *       S    *= exp(g)
 *       S    += k (x) (v - S^T k) * beta
 *       out   = S^T q / sqrt(head_dim)
 *   out = RMSNorm(out, ssm_norm) * SiLU(z), then ssm_out
 *
 * Numerics follow llama.cpp's reference graph exactly, including the
 * 1/sqrt(head_dim) scaling of the delta-net readout and the shared
 * (grouped) k-heads being broadcast across v-heads.
 *
 * ANSI C (C89), scalar arithmetic only, single-threaded: this is the
 * i486 target. All buffers are allocated once at context creation.
 */

#include "infer.h"
#include "prof.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Hyper-parameters                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int   n_layer;
    int   n_embd;
    int   n_ff;
    int   n_head;
    int   n_head_kv;
    int   head_dim;        /* attention.key_length */
    int   n_rot;           /* rope.dimension_count */
    int   rope_sec[4];
    float rope_base;
    float rms_eps;

    /* delta-net */
    int   ssm_conv;        /* conv kernel width */
    int   ssm_state;       /* head_k_dim == head_v_dim */
    int   ssm_group;       /* number of k heads */
    int   ssm_dt_rank;     /* number of v heads */
    int   ssm_inner;       /* v heads * head_v_dim */
    int   full_attn_int;

    int   n_vocab;
    int   n_ctx_train;
} hparams;

/* ------------------------------------------------------------------ */
/* Per-layer weights                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int is_recurrent;

    gg_tensor *attn_norm;
    gg_tensor *post_norm;

    /* full attention */
    gg_tensor *wq, *wk, *wv, *wo;
    gg_tensor *q_norm, *k_norm;

    /* delta net */
    gg_tensor *qkv, *gate;      /* attn_qkv, attn_gate */
    gg_tensor *conv1d;          /* [kernel, conv_dim] f32 */
    gg_tensor *dt_bias;         /* [n_v_heads] f32 */
    gg_tensor *a;               /* [n_v_heads] f32 */
    gg_tensor *beta, *alpha;    /* [n_embd, n_v_heads] */
    gg_tensor *ssm_norm;        /* [head_v_dim] f32 */
    gg_tensor *ssm_out;         /* [value_dim, n_embd] */

    /* ffn */
    gg_tensor *ffn_gate, *ffn_up, *ffn_down;
} layer;

struct qwen35_model {
    gguf_file  gguf;
    hparams    hp;
    layer     *layers;
    gg_tensor *tok_embd;
    gg_tensor *output_norm;
    gg_tensor *output;      /* may alias tok_embd (tied embeddings) */
    tokenizer *tok;
    char       name[128];
};

/* ------------------------------------------------------------------ */
/* Context: activations + recurrent state + KV cache                   */
/* ------------------------------------------------------------------ */

struct qwen35_ctx {
    qwen35_model *m;
    int n_ctx;

    /* activation scratch */
    float *x;        /* [n_embd]  residual stream          */
    float *xn;       /* [n_embd]  normalised input         */
    float *tmp;      /* [n_embd]  mix output               */
    float *ffn_a;    /* [n_ff]                             */
    float *ffn_b;    /* [n_ff]                             */
    float *logits;   /* [n_vocab]                          */

    /* attention scratch */
    float *qbuf;     /* [2 * head_dim * n_head]            */
    float *kbuf;     /* [head_dim * n_head_kv]             */
    float *vbuf;     /* [head_dim * n_head_kv]             */
    float *att;      /* [n_ctx]  scores for one head       */
    float *aout;     /* [head_dim * n_head]                */

    /* KV cache: one entry per full-attention layer */
    int    n_attn_layers;
    int   *attn_slot;      /* layer index -> cache slot, or -1  */
    float *kcache;         /* [slots][n_ctx][n_head_kv*head_dim] */
    float *vcache;

    /* delta-net scratch */
    float *qkv;      /* [conv_dim]                         */
    float *zbuf;     /* [value_dim]                        */
    float *dn_out;   /* [value_dim]                        */
    float *betav;    /* [n_v_heads]                        */
    float *alphav;   /* [n_v_heads]                        */
    float *kv_delta; /* [head_dim]                         */

    /* recurrent state: one per delta-net layer */
    int    n_recr_layers;
    int   *recr_slot;      /* layer index -> slot, or -1        */
    float *conv_state;     /* [slots][conv_dim][conv_kernel-1]  */
    float *ssm_state;      /* [slots][n_v_heads][hd][hd]        */

    int pos_max;
};

/* ------------------------------------------------------------------ */
/* Small math helpers                                                  */
/* ------------------------------------------------------------------ */

static void rms_norm(const float *x, const float *w, float *y,
                     int n, float eps) {
    float ss = 0.0f;
    int i;
    for (i = 0; i < n; i++) ss += x[i] * x[i];
    ss = 1.0f / (float) sqrt((double) (ss / (float) n) + (double) eps);
    for (i = 0; i < n; i++) y[i] = x[i] * ss * w[i];
}

static void l2_norm(float *x, int n, float eps) {
    float ss = 0.0f;
    int i;
    for (i = 0; i < n; i++) ss += x[i] * x[i];
    ss = 1.0f / (float) sqrt((double) ss + (double) eps);
    for (i = 0; i < n; i++) x[i] *= ss;
}

static float silu(float v) {
    return v / (1.0f + (float) exp(-(double) v));
}

static float sigmoidf_(float v) {
    return 1.0f / (1.0f + (float) exp(-(double) v));
}

static float softplusf_(float v) {
    /* log1p(exp(v)) with the standard large-v guard */
    if (v > 20.0f) return v;
    return (float) log(1.0 + exp((double) v));
}

static void softmax_inplace(float *x, int n) {
    float mx = x[0], sum = 0.0f;
    int i;
    for (i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    for (i = 0; i < n; i++) {
        x[i] = (float) exp((double) (x[i] - mx));
        sum += x[i];
    }
    sum = 1.0f / sum;
    for (i = 0; i < n; i++) x[i] *= sum;
}

/* Read a whole f32 tensor as floats (used for the small norm vectors).
 *
 * gguf_f32data() returns the mapping directly on a little-endian host;
 * on a big-endian one it hands back a swapped copy made once at first
 * use. Either way this is not on the hot path -- F32 tensors are the
 * norm/conv/SSM vectors, ~0.07% of the weights in a k-quant model. */
static const float *f32data(gg_tensor *t) {
    return gguf_f32data(t);
}

/* y[nrows] = W x, W = t (row length t->ne[0]) */
static void matvec_t(gg_tensor *t, const float *x, float *y) {
    q_matvec(t->type, t->data, x, y, t->ne[0], t->ne[1]);
}

/* ------------------------------------------------------------------ */
/* Model loading                                                       */
/* ------------------------------------------------------------------ */

static gg_tensor *need(gguf_file *g, const char *fmt, int il,
                       int required, char *err, size_t errlen) {
    char nm[64];
    gg_tensor *t;
    if (il >= 0) sprintf(nm, fmt, il);
    else         strcpy(nm, fmt);
    t = gguf_tensor(g, nm);
    if (!t && required && err && !err[0]) {
        sprintf(err, "missing tensor '%.48s'", nm);
    }
    return t;
}

qwen35_model *qwen35_load(const char *path, char *errbuf, size_t errlen) {
    qwen35_model *m;
    hparams *hp;
    const char *arch;
    const gg_kv *secs;
    int il;

    if (errbuf && errlen) errbuf[0] = '\0';

    m = (qwen35_model *) xcalloc(1, sizeof(qwen35_model));
    if (gguf_open(&m->gguf, path) != 0) {
        if (errbuf) strcpy(errbuf, "cannot read GGUF file");
        free(m);
        return NULL;
    }

    arch = gguf_str(&m->gguf, "general.architecture");
    if (!arch || strcmp(arch, "qwen35") != 0) {
        if (errbuf) {
            sprintf(errbuf, "architecture '%.32s' is not supported "
                            "(this build implements 'qwen35')",
                    arch ? arch : "?");
        }
        gguf_close(&m->gguf);
        free(m);
        return NULL;
    }

    {
        const char *nm = gguf_str(&m->gguf, "general.name");
        strncpy(m->name, nm ? nm : "qwen35", sizeof(m->name) - 1);
    }

    hp = &m->hp;
    hp->n_layer     = (int) gguf_num(&m->gguf, "qwen35.block_count", 24);
    hp->n_embd      = (int) gguf_num(&m->gguf, "qwen35.embedding_length", 1024);
    hp->n_ff        = (int) gguf_num(&m->gguf, "qwen35.feed_forward_length", 3584);
    hp->n_head      = (int) gguf_num(&m->gguf, "qwen35.attention.head_count", 8);
    hp->n_head_kv   = (int) gguf_num(&m->gguf, "qwen35.attention.head_count_kv", 2);
    hp->head_dim    = (int) gguf_num(&m->gguf, "qwen35.attention.key_length", 256);
    hp->n_rot       = (int) gguf_num(&m->gguf, "qwen35.rope.dimension_count", 64);
    hp->rope_base   = (float) gguf_num(&m->gguf, "qwen35.rope.freq_base", 10000000.0);
    hp->rms_eps     = (float) gguf_num(&m->gguf,
                        "qwen35.attention.layer_norm_rms_epsilon", 1e-6);
    hp->ssm_conv    = (int) gguf_num(&m->gguf, "qwen35.ssm.conv_kernel", 4);
    hp->ssm_state   = (int) gguf_num(&m->gguf, "qwen35.ssm.state_size", 128);
    hp->ssm_group   = (int) gguf_num(&m->gguf, "qwen35.ssm.group_count", 16);
    hp->ssm_dt_rank = (int) gguf_num(&m->gguf, "qwen35.ssm.time_step_rank", 16);
    hp->ssm_inner   = (int) gguf_num(&m->gguf, "qwen35.ssm.inner_size", 2048);
    hp->full_attn_int = (int) gguf_num(&m->gguf,
                          "qwen35.full_attention_interval", 4);
    hp->n_ctx_train = (int) gguf_num(&m->gguf, "qwen35.context_length", 32768);

    secs = gguf_find(&m->gguf, "qwen35.rope.dimension_sections");
    if (secs && secs->type == GGUF_TYPE_ARRAY && secs->arr_n >= 4 &&
        secs->arr_type != GGUF_TYPE_STRING) {
        const float *v = (const float *) secs->arr;
        int i;
        for (i = 0; i < 4; i++) hp->rope_sec[i] = (int) v[i];
    } else {
        hp->rope_sec[0] = hp->rope_sec[1] = hp->rope_sec[2] = hp->rope_sec[3] = 0;
    }

    m->tok = tok_create(&m->gguf);
    if (!m->tok) {
        if (errbuf) strcpy(errbuf, "model has no usable tokenizer");
        gguf_close(&m->gguf);
        free(m);
        return NULL;
    }
    hp->n_vocab = tok_n_vocab(m->tok);

    /* ---- global tensors ---- */
    {
        char err[128];
        err[0] = '\0';
        m->tok_embd    = need(&m->gguf, "token_embd.weight",  -1, 1, err, sizeof(err));
        m->output_norm = need(&m->gguf, "output_norm.weight", -1, 1, err, sizeof(err));
        m->output      = gguf_tensor(&m->gguf, "output.weight");
        if (!m->output) m->output = m->tok_embd;   /* tied embeddings */

        if (err[0]) {
            if (errbuf) strncpy(errbuf, err, errlen - 1);
            tok_free(m->tok);
            gguf_close(&m->gguf);
            free(m);
            return NULL;
        }
    }

    /* ---- per-layer tensors ---- */
    m->layers = (layer *) xcalloc((size_t) hp->n_layer, sizeof(layer));
    for (il = 0; il < hp->n_layer; il++) {
        layer *L = &m->layers[il];
        char err[128];
        err[0] = '\0';

        L->is_recurrent = ((il + 1) % hp->full_attn_int) != 0;

        L->attn_norm = need(&m->gguf, "blk.%d.attn_norm.weight", il, 1, err, sizeof(err));
        L->post_norm = need(&m->gguf, "blk.%d.post_attention_norm.weight", il, 1, err, sizeof(err));

        if (L->is_recurrent) {
            L->qkv      = need(&m->gguf, "blk.%d.attn_qkv.weight",   il, 1, err, sizeof(err));
            L->gate     = need(&m->gguf, "blk.%d.attn_gate.weight",  il, 1, err, sizeof(err));
            L->conv1d   = need(&m->gguf, "blk.%d.ssm_conv1d.weight", il, 1, err, sizeof(err));
            L->dt_bias  = need(&m->gguf, "blk.%d.ssm_dt.bias",       il, 1, err, sizeof(err));
            L->a        = need(&m->gguf, "blk.%d.ssm_a",             il, 1, err, sizeof(err));
            L->beta     = need(&m->gguf, "blk.%d.ssm_beta.weight",   il, 1, err, sizeof(err));
            L->alpha    = need(&m->gguf, "blk.%d.ssm_alpha.weight",  il, 1, err, sizeof(err));
            L->ssm_norm = need(&m->gguf, "blk.%d.ssm_norm.weight",   il, 1, err, sizeof(err));
            L->ssm_out  = need(&m->gguf, "blk.%d.ssm_out.weight",    il, 1, err, sizeof(err));
        } else {
            L->wq     = need(&m->gguf, "blk.%d.attn_q.weight",      il, 1, err, sizeof(err));
            L->wk     = need(&m->gguf, "blk.%d.attn_k.weight",      il, 1, err, sizeof(err));
            L->wv     = need(&m->gguf, "blk.%d.attn_v.weight",      il, 1, err, sizeof(err));
            L->wo     = need(&m->gguf, "blk.%d.attn_output.weight", il, 1, err, sizeof(err));
            L->q_norm = need(&m->gguf, "blk.%d.attn_q_norm.weight", il, 1, err, sizeof(err));
            L->k_norm = need(&m->gguf, "blk.%d.attn_k_norm.weight", il, 1, err, sizeof(err));
        }

        L->ffn_gate = need(&m->gguf, "blk.%d.ffn_gate.weight", il, 1, err, sizeof(err));
        L->ffn_up   = need(&m->gguf, "blk.%d.ffn_up.weight",   il, 1, err, sizeof(err));
        L->ffn_down = need(&m->gguf, "blk.%d.ffn_down.weight", il, 1, err, sizeof(err));

        if (err[0]) {
            if (errbuf) strncpy(errbuf, err, errlen - 1);
            qwen35_free(m);
            return NULL;
        }
    }

    return m;
}

void qwen35_free(qwen35_model *m) {
    if (!m) return;
    if (m->tok)    tok_free(m->tok);
    if (m->layers) free(m->layers);
    gguf_close(&m->gguf);
    free(m);
}

tokenizer  *qwen35_tokenizer(qwen35_model *m)  { return m->tok; }
const char *qwen35_name(qwen35_model *m)       { return m->name; }
int         qwen35_n_vocab(qwen35_model *m)    { return m->hp.n_vocab; }
int         qwen35_n_ctx_train(qwen35_model *m){ return m->hp.n_ctx_train; }
const char *qwen35_chat_template(qwen35_model *m) {
    return gguf_str(&m->gguf, "tokenizer.chat_template");
}
int         qwen35_n_ctx(qwen35_ctx *c)        { return c->n_ctx; }

/* ------------------------------------------------------------------ */
/* Context allocation                                                  */
/* ------------------------------------------------------------------ */

qwen35_ctx *qwen35_ctx_create(qwen35_model *m, const qwen35_params *p) {
    qwen35_ctx *c;
    hparams *hp = &m->hp;
    int il;
    int key_dim   = hp->ssm_state * hp->ssm_group;
    int value_dim = hp->ssm_state * hp->ssm_dt_rank;
    int conv_dim  = key_dim * 2 + value_dim;
    int hd        = hp->ssm_state;

    c = (qwen35_ctx *) xcalloc(1, sizeof(qwen35_ctx));
    c->m = m;
    c->n_ctx = (p && p->n_ctx > 0) ? p->n_ctx : 4096;

    c->x      = (float *) xmalloc(sizeof(float) * (size_t) hp->n_embd);
    c->xn     = (float *) xmalloc(sizeof(float) * (size_t) hp->n_embd);
    c->tmp    = (float *) xmalloc(sizeof(float) * (size_t) hp->n_embd);
    c->ffn_a  = (float *) xmalloc(sizeof(float) * (size_t) hp->n_ff);
    c->ffn_b  = (float *) xmalloc(sizeof(float) * (size_t) hp->n_ff);
    c->logits = (float *) xmalloc(sizeof(float) * (size_t) hp->n_vocab);

    c->qbuf = (float *) xmalloc(sizeof(float) *
                (size_t) (2 * hp->head_dim * hp->n_head));
    c->kbuf = (float *) xmalloc(sizeof(float) *
                (size_t) (hp->head_dim * hp->n_head_kv));
    c->vbuf = (float *) xmalloc(sizeof(float) *
                (size_t) (hp->head_dim * hp->n_head_kv));
    c->att  = (float *) xmalloc(sizeof(float) * (size_t) c->n_ctx);
    c->aout = (float *) xmalloc(sizeof(float) *
                (size_t) (hp->head_dim * hp->n_head));

    c->qkv      = (float *) xmalloc(sizeof(float) * (size_t) conv_dim);
    c->zbuf     = (float *) xmalloc(sizeof(float) * (size_t) value_dim);
    c->dn_out   = (float *) xmalloc(sizeof(float) * (size_t) value_dim);
    c->betav    = (float *) xmalloc(sizeof(float) * (size_t) hp->ssm_dt_rank);
    c->alphav   = (float *) xmalloc(sizeof(float) * (size_t) hp->ssm_dt_rank);
    c->kv_delta = (float *) xmalloc(sizeof(float) * (size_t) hd);

    /* slot maps */
    c->attn_slot = (int *) xmalloc(sizeof(int) * (size_t) hp->n_layer);
    c->recr_slot = (int *) xmalloc(sizeof(int) * (size_t) hp->n_layer);
    c->n_attn_layers = 0;
    c->n_recr_layers = 0;
    for (il = 0; il < hp->n_layer; il++) {
        if (m->layers[il].is_recurrent) {
            c->attn_slot[il] = -1;
            c->recr_slot[il] = c->n_recr_layers++;
        } else {
            c->recr_slot[il] = -1;
            c->attn_slot[il] = c->n_attn_layers++;
        }
    }

    /* KV cache */
    {
        size_t per = (size_t) c->n_ctx * (size_t) (hp->n_head_kv * hp->head_dim);
        c->kcache = (float *) xcalloc((size_t) c->n_attn_layers * per,
                                      sizeof(float));
        c->vcache = (float *) xcalloc((size_t) c->n_attn_layers * per,
                                      sizeof(float));
    }

    /* recurrent state */
    c->conv_state = (float *) xcalloc(
        (size_t) c->n_recr_layers * (size_t) conv_dim *
        (size_t) (hp->ssm_conv - 1), sizeof(float));
    c->ssm_state = (float *) xcalloc(
        (size_t) c->n_recr_layers * (size_t) hp->ssm_dt_rank *
        (size_t) hd * (size_t) hd, sizeof(float));

    c->pos_max = 0;
    return c;
}

void qwen35_ctx_free(qwen35_ctx *c) {
    if (!c) return;
    free(c->x); free(c->xn); free(c->tmp);
    free(c->ffn_a); free(c->ffn_b); free(c->logits);
    free(c->qbuf); free(c->kbuf); free(c->vbuf);
    free(c->att); free(c->aout);
    free(c->qkv); free(c->zbuf); free(c->dn_out);
    free(c->betav); free(c->alphav); free(c->kv_delta);
    free(c->attn_slot); free(c->recr_slot);
    free(c->kcache); free(c->vcache);
    free(c->conv_state); free(c->ssm_state);
    free(c);
}

void qwen35_reset(qwen35_ctx *c) {
    hparams *hp = &c->m->hp;
    int key_dim   = hp->ssm_state * hp->ssm_group;
    int value_dim = hp->ssm_state * hp->ssm_dt_rank;
    int conv_dim  = key_dim * 2 + value_dim;
    int hd        = hp->ssm_state;

    memset(c->conv_state, 0,
           (size_t) c->n_recr_layers * (size_t) conv_dim *
           (size_t) (hp->ssm_conv - 1) * sizeof(float));
    memset(c->ssm_state, 0,
           (size_t) c->n_recr_layers * (size_t) hp->ssm_dt_rank *
           (size_t) hd * (size_t) hd * sizeof(float));
    c->pos_max = 0;
}

/* ------------------------------------------------------------------ */
/* Interleaved multimodal RoPE                                         */
/*                                                                     */
/* For text-only input all three positional axes (t, h, w) carry the    */
/* same value, so i-mRoPE reduces to ordinary NeoX-style RoPE over the  */
/* first n_rot dimensions. We implement exactly that.                   */
/* ------------------------------------------------------------------ */

static void rope_apply(float *v, int head_dim, int n_rot, int pos,
                       float base) {
    int i;
    int half = n_rot / 2;

    for (i = 0; i < half; i++) {
        float inv = (float) pow((double) base,
                                -(double) (2 * i) / (double) n_rot);
        float ang = (float) pos * inv;
        float cs = (float) cos((double) ang);
        float sn = (float) sin((double) ang);
        float a = v[i];
        float b = v[i + half];
        v[i]        = a * cs - b * sn;
        v[i + half] = a * sn + b * cs;
    }
    (void) head_dim;   /* dims >= n_rot are passed through unrotated */
}

/* ------------------------------------------------------------------ */
/* Full attention layer                                                */
/* ------------------------------------------------------------------ */

static void layer_attention(qwen35_ctx *c, layer *L, int il, int pos) {
    qwen35_model *m = c->m;
    hparams *hp = &m->hp;
    int hd   = hp->head_dim;
    int nh   = hp->n_head;
    int nkv  = hp->n_head_kv;
    int gqa  = nh / nkv;
    int slot = c->attn_slot[il];
    size_t kv_row = (size_t) (nkv * hd);
    size_t base   = (size_t) slot * (size_t) c->n_ctx * kv_row;
    float scale = 1.0f / (float) sqrt((double) hd);
    int h, i, t;

    PROF_DECL(pt);

    /* Q projection produces [q | gate] interleaved per head. */
    PROF_START(pt);
    matvec_t(L->wq, c->xn, c->qbuf);
    matvec_t(L->wk, c->xn, c->kbuf);
    matvec_t(L->wv, c->xn, c->vbuf);
    PROF_STOP(pt, PF_ATTN_QKV, 0);

    /* per-head RMS norm + RoPE on Q */
    for (h = 0; h < nh; h++) {
        float *q = c->qbuf + (size_t) h * 2 * hd;   /* gate follows at +hd */
        rms_norm(q, f32data(L->q_norm), q, hd, hp->rms_eps);
        rope_apply(q, hd, hp->n_rot, pos, hp->rope_base);
    }
    /* per-head RMS norm + RoPE on K */
    for (h = 0; h < nkv; h++) {
        float *k = c->kbuf + (size_t) h * hd;
        rms_norm(k, f32data(L->k_norm), k, hd, hp->rms_eps);
        rope_apply(k, hd, hp->n_rot, pos, hp->rope_base);
    }

    /* append K/V to the cache at this position */
    {
        float *kdst = c->kcache + base + (size_t) pos * kv_row;
        float *vdst = c->vcache + base + (size_t) pos * kv_row;
        memcpy(kdst, c->kbuf, kv_row * sizeof(float));
        memcpy(vdst, c->vbuf, kv_row * sizeof(float));
    }

    /* causal attention, one query head at a time */
    PROF_START(pt);
    for (h = 0; h < nh; h++) {
        const float *q = c->qbuf + (size_t) h * 2 * hd;
        int kvh = h / gqa;
        float *o = c->aout + (size_t) h * hd;

        for (t = 0; t <= pos; t++) {
            const float *k = c->kcache + base + (size_t) t * kv_row +
                             (size_t) kvh * hd;
            float s = 0.0f;
            for (i = 0; i < hd; i++) s += q[i] * k[i];
            c->att[t] = s * scale;
        }
        softmax_inplace(c->att, pos + 1);

        for (i = 0; i < hd; i++) o[i] = 0.0f;
        for (t = 0; t <= pos; t++) {
            const float *v = c->vcache + base + (size_t) t * kv_row +
                             (size_t) kvh * hd;
            float a = c->att[t];
            for (i = 0; i < hd; i++) o[i] += a * v[i];
        }

        /* output gate: sigmoid of the second half of this head's Q block */
        {
            const float *g = c->qbuf + (size_t) h * 2 * hd + hd;
            for (i = 0; i < hd; i++) o[i] *= sigmoidf_(g[i]);
        }
    }

    PROF_STOP(pt, PF_ATTN_SCORE, 0);

    PROF_START(pt);
    matvec_t(L->wo, c->aout, c->tmp);
    PROF_STOP(pt, PF_ATTN_OUT, 0);
}

/* ------------------------------------------------------------------ */
/* Gated DeltaNet layer                                                */
/* ------------------------------------------------------------------ */

static void layer_deltanet(qwen35_ctx *c, layer *L, int il) {
    qwen35_model *m = c->m;
    hparams *hp = &m->hp;
    int hd        = hp->ssm_state;       /* head_k_dim == head_v_dim */
    int n_kh      = hp->ssm_group;       /* k/q heads */
    int n_vh      = hp->ssm_dt_rank;     /* v heads   */
    int key_dim   = hd * n_kh;
    int value_dim = hd * n_vh;
    int conv_dim  = key_dim * 2 + value_dim;
    int kw        = hp->ssm_conv;
    int slot      = c->recr_slot[il];
    float scale   = 1.0f / (float) sqrt((double) hd);
    int i, j, h;

    float *conv_st = c->conv_state +
                     (size_t) slot * (size_t) conv_dim * (size_t) (kw - 1);
    float *st_base = c->ssm_state +
                     (size_t) slot * (size_t) n_vh * (size_t) hd * (size_t) hd;

    PROF_DECL(pt);

    /* ---- projections ---- */
    PROF_START(pt);
    matvec_t(L->qkv,  c->xn, c->qkv);    /* [q | k | v] */
    matvec_t(L->gate, c->xn, c->zbuf);   /* z */
    matvec_t(L->beta,  c->xn, c->betav);
    matvec_t(L->alpha, c->xn, c->alphav);
    PROF_STOP(pt, PF_DN_PROJ, 0);

    /* ---- causal depthwise conv (width kw) + SiLU, per channel ---- */
    PROF_START(pt);
    {
        const float *w = f32data(L->conv1d);   /* [kw, conv_dim] */
        for (i = 0; i < conv_dim; i++) {
            const float *wc = w + (size_t) i * kw;
            float *hist = conv_st + (size_t) i * (kw - 1);
            float acc = 0.0f;

            /* history holds the previous kw-1 inputs, oldest first */
            for (j = 0; j < kw - 1; j++) acc += wc[j] * hist[j];
            acc += wc[kw - 1] * c->qkv[i];

            /* shift history */
            for (j = 0; j < kw - 2; j++) hist[j] = hist[j + 1];
            if (kw >= 2) hist[kw - 2] = c->qkv[i];

            c->qkv[i] = silu(acc);
        }
    }
    PROF_STOP(pt, PF_DN_CONV, 0);

    /* ---- L2-normalise q and k per head ---- */
    for (h = 0; h < n_kh; h++) {
        l2_norm(c->qkv + (size_t) h * hd, hd, hp->rms_eps);
        l2_norm(c->qkv + key_dim + (size_t) h * hd, hd, hp->rms_eps);
    }

    /* ---- gates ---- */
    {
        const float *dt = f32data(L->dt_bias);
        const float *av = f32data(L->a);
        for (h = 0; h < n_vh; h++) {
            c->betav[h]  = sigmoidf_(c->betav[h]);
            /* g = A * softplus(alpha + dt_bias); ssm_a is already negative
             * in the GGUF (it stores -exp(A_log)), so exp(g) decays. */
            c->alphav[h] = av[h] * softplusf_(c->alphav[h] + dt[h]);
        }
    }

    /* ---- recurrence, per value head ---- */
    PROF_START(pt);
    for (h = 0; h < n_vh; h++) {
        /* k-heads are shared across v-heads in groups */
        int kh = h % n_kh;
        const float *q = c->qkv + (size_t) kh * hd;
        const float *k = c->qkv + key_dim + (size_t) kh * hd;
        const float *v = c->qkv + 2 * key_dim + (size_t) h * hd;
        float *S = st_base + (size_t) h * (size_t) hd * (size_t) hd;
        float *o = c->dn_out + (size_t) h * hd;
        float decay = (float) exp((double) c->alphav[h]);
        float beta  = c->betav[h];

        /* S is stored transposed: S[j*hd + i] == state[i][j], so row j is
         * contiguous and every inner loop below is a unit-stride dot. */

        /* S is 128x128 floats = 64 KB per head -- exactly the size of
         * the Geode's L1 data cache. The four logical steps below used
         * to be four separate full passes over S, so S was streamed
         * through L1 four times per head and never stayed resident.
         *
         * Steps 1+2 are fused (decay a row, then immediately dot it
         * with k) and steps 3+4 are fused (update a row, then dot it
         * with q). That halves the traffic over S: two passes instead
         * of four, and each row is touched while it is still hot. */

        /* pass A: decay each row, then delta[j] = (v[j] - <row,k>)*beta */
        for (j = 0; j < hd; j++) {
            float *row = S + (size_t) j * hd;
            float s = 0.0f;
            for (i = 0; i < hd; i++) {
                float e = row[i] * decay;
                row[i] = e;
                s += e * k[i];
            }
            c->kv_delta[j] = (v[j] - s) * beta;
        }

        /* pass B: rank-1 update, then readout o[j] = <row,q>/sqrt(hd) */
        for (j = 0; j < hd; j++) {
            float *row = S + (size_t) j * hd;
            float d = c->kv_delta[j];
            float s = 0.0f;
            for (i = 0; i < hd; i++) {
                float e = row[i] + d * k[i];
                row[i] = e;
                s += e * q[i];
            }
            o[j] = s * scale;
        }
    }

    PROF_STOP(pt, PF_DN_RECUR, (double) n_vh * hd * hd * 3.0);

    /* ---- gated RMS norm (per head) then output projection ---- */
    PROF_START(pt);
    {
        const float *gw = f32data(L->ssm_norm);
        for (h = 0; h < n_vh; h++) {
            float *o = c->dn_out + (size_t) h * hd;
            const float *z = c->zbuf + (size_t) h * hd;
            rms_norm(o, gw, o, hd, hp->rms_eps);
            for (i = 0; i < hd; i++) o[i] *= silu(z[i]);
        }
    }

    matvec_t(L->ssm_out, c->dn_out, c->tmp);
    PROF_STOP(pt, PF_DN_OUT, 0);
    (void) value_dim;
}

/* ------------------------------------------------------------------ */
/* Feed-forward (SwiGLU)                                               */
/* ------------------------------------------------------------------ */

static void layer_ffn(qwen35_ctx *c, layer *L) {
    hparams *hp = &c->m->hp;
    int i;
    PROF_DECL(pt);

    PROF_START(pt);
    matvec_t(L->ffn_gate, c->xn, c->ffn_a);
    matvec_t(L->ffn_up,   c->xn, c->ffn_b);
    for (i = 0; i < hp->n_ff; i++) c->ffn_a[i] = silu(c->ffn_a[i]) * c->ffn_b[i];
    matvec_t(L->ffn_down, c->ffn_a, c->tmp);
    PROF_STOP(pt, PF_FFN, 0);
}

/* ------------------------------------------------------------------ */
/* One decode step                                                     */
/* ------------------------------------------------------------------ */

float *qwen35_decode(qwen35_ctx *c, int token, int pos, int want_logits) {
    qwen35_model *m = c->m;
    hparams *hp = &m->hp;
    int il, i;
    PROF_DECL(pt);
    PROF_DECL(ptot);
    PROF_DECL(pn);

    PROF_START(ptot);

    if (pos >= c->n_ctx) pos = c->n_ctx - 1;

    /* ---- embedding lookup ---- */
    PROF_START(pt);
    {
        gg_tensor *e = m->tok_embd;
        long rowbytes = gg_type_size(e->type, e->ne[0]);
        if (token < 0 || token >= hp->n_vocab) token = 0;
        q_dequant_row(e->type, e->data + (size_t) token * rowbytes,
                      c->x, hp->n_embd);
    }
    PROF_STOP(pt, PF_EMBED, 0);

    /* ---- transformer blocks ---- */
    for (il = 0; il < hp->n_layer; il++) {
        layer *L = &m->layers[il];

        PROF_START(pn);
        rms_norm(c->x, f32data(L->attn_norm), c->xn, hp->n_embd, hp->rms_eps);
        PROF_STOP(pn, PF_NORM, 0);

        PROF_START(pt);
        if (L->is_recurrent) layer_deltanet(c, L, il);
        else                 layer_attention(c, L, il, pos);
        PROF_STOP(pt, L->is_recurrent ? PF_DELTANET : PF_ATTN, 0);

        for (i = 0; i < hp->n_embd; i++) c->x[i] += c->tmp[i];

        PROF_START(pn);
        rms_norm(c->x, f32data(L->post_norm), c->xn, hp->n_embd, hp->rms_eps);
        PROF_STOP(pn, PF_NORM, 0);
        layer_ffn(c, L);

        for (i = 0; i < hp->n_embd; i++) c->x[i] += c->tmp[i];
    }

    if (pos + 1 > c->pos_max) c->pos_max = pos + 1;

    if (!want_logits) {
        PROF_STOP(ptot, PF_TOTAL, 0);
        return NULL;
    }

    PROF_START(pt);
    rms_norm(c->x, f32data(m->output_norm), c->xn, hp->n_embd, hp->rms_eps);
    matvec_t(m->output, c->xn, c->logits);
    PROF_STOP(pt, PF_LMHEAD, 0);

    PROF_STOP(ptot, PF_TOTAL, 0);
    return c->logits;
}
