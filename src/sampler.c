/* sampler.c -- logit post-processing and token selection.
 *
 * Supports greedy decoding, temperature, top-k, top-p (nucleus) and a
 * repetition penalty over a sliding window of recent tokens.
 *
 * ANSI C (C89). The RNG is a self-contained xorshift so behaviour is
 * identical on every platform and no libc rand() quality assumptions
 * are made.
 */

#include "infer.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    float v;
    int   id;
} cand;

struct sampler {
    sampler_params p;
    int   n_vocab;
    cand *cands;
    int  *recent;      /* ring buffer of accepted tokens */
    int   n_recent;
    int   recent_pos;
    unsigned long rng;
};

void sampler_default(sampler_params *p) {
    p->temperature    = 0.7f;
    p->top_p          = 0.8f;
    p->top_k          = 40;
    p->repeat_penalty = 1.05f;
    p->repeat_last_n  = 64;
    p->seed           = 0;   /* 0 = derive from time */
}

sampler *sampler_create(const sampler_params *p, int n_vocab) {
    sampler *s = (sampler *) xcalloc(1, sizeof(sampler));
    s->p       = *p;
    s->n_vocab = n_vocab;
    s->cands   = (cand *) xmalloc(sizeof(cand) * (size_t) n_vocab);

    if (s->p.repeat_last_n < 0) s->p.repeat_last_n = 0;
    s->recent = (int *) xmalloc(sizeof(int) *
                                (size_t) (s->p.repeat_last_n > 0
                                          ? s->p.repeat_last_n : 1));
    s->n_recent   = 0;
    s->recent_pos = 0;

    /* Must fit in 32 bits: on i486 `unsigned long` is exactly 32 bits,
     * and C89 has no wider integer type. */
    s->rng = p->seed ? (p->seed & 0xFFFFFFFFUL) : 2463534242UL;
    if (s->rng == 0) s->rng = 1;
    return s;
}

void sampler_free(sampler *s) {
    if (!s) return;
    free(s->cands);
    free(s->recent);
    free(s);
}

void sampler_reset(sampler *s) {
    s->n_recent   = 0;
    s->recent_pos = 0;
}

void sampler_accept(sampler *s, int token) {
    if (s->p.repeat_last_n <= 0) return;
    s->recent[s->recent_pos] = token;
    s->recent_pos = (s->recent_pos + 1) % s->p.repeat_last_n;
    if (s->n_recent < s->p.repeat_last_n) s->n_recent++;
}

/* 32-bit xorshift */
static unsigned long rng_next(sampler *s) {
    unsigned long x = s->rng;
    x ^= (x << 13) & 0xFFFFFFFFUL;
    x ^= (x >> 17);
    x ^= (x << 5) & 0xFFFFFFFFUL;
    s->rng = x & 0xFFFFFFFFUL;
    return s->rng;
}

static float rng_float(sampler *s) {
    return (float) rng_next(s) / 4294967296.0f;
}

/* Descending sort by value. Introsort-ish: quicksort with insertion
 * sort for short runs, so worst-case behaviour stays acceptable without
 * pulling in qsort's callback overhead. */
static void sort_desc(cand *a, int n) {
    while (n > 12) {
        cand pivot = a[n / 2];
        int i = 0, j = n - 1;
        while (i <= j) {
            while (a[i].v > pivot.v) i++;
            while (a[j].v < pivot.v) j--;
            if (i <= j) {
                cand t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (j > 0) sort_desc(a, j + 1);
        a += i;
        n -= i;
    }
    {
        int i, j;
        for (i = 1; i < n; i++) {
            cand key = a[i];
            j = i - 1;
            while (j >= 0 && a[j].v < key.v) {
                a[j + 1] = a[j];
                j--;
            }
            a[j + 1] = key;
        }
    }
}

int sampler_pick(sampler *s, float *logits) {
    int n = s->n_vocab;
    int i, k;

    /* ---- repetition penalty ---- */
    if (s->p.repeat_penalty != 1.0f && s->n_recent > 0) {
        for (i = 0; i < s->n_recent; i++) {
            int id = s->recent[i];
            if (id < 0 || id >= n) continue;
            if (logits[id] > 0.0f) logits[id] /= s->p.repeat_penalty;
            else                   logits[id] *= s->p.repeat_penalty;
        }
    }

    /* ---- greedy shortcut ---- */
    if (s->p.temperature <= 0.0f) {
        int best = 0;
        float bv = logits[0];
        for (i = 1; i < n; i++) {
            if (logits[i] > bv) { bv = logits[i]; best = i; }
        }
        return best;
    }

    /* ---- build candidate list ---- */
    for (i = 0; i < n; i++) {
        s->cands[i].v  = logits[i];
        s->cands[i].id = i;
    }

    /* ---- top-k ---- */
    k = s->p.top_k;
    if (k <= 0 || k > n) k = n;

    if (k < n) {
        /* Partial selection: repeatedly extract the max into the front.
         * For the small k values used in practice this beats a full sort. */
        int a;
        for (a = 0; a < k; a++) {
            int best = a, b;
            for (b = a + 1; b < n; b++) {
                if (s->cands[b].v > s->cands[best].v) best = b;
            }
            if (best != a) {
                cand t = s->cands[a];
                s->cands[a] = s->cands[best];
                s->cands[best] = t;
            }
        }
    } else {
        sort_desc(s->cands, n);
    }

    /* ---- softmax over the k survivors ---- */
    {
        float mx = s->cands[0].v;
        float sum = 0.0f;
        float inv_t = 1.0f / s->p.temperature;
        for (i = 0; i < k; i++) {
            s->cands[i].v = (float) exp((double) ((s->cands[i].v - mx) * inv_t));
            sum += s->cands[i].v;
        }
        for (i = 0; i < k; i++) s->cands[i].v /= sum;
    }

    /* ---- top-p (nucleus) ---- */
    if (s->p.top_p < 1.0f) {
        float cum = 0.0f;
        for (i = 0; i < k; i++) {
            cum += s->cands[i].v;
            if (cum >= s->p.top_p) {
                k = i + 1;
                break;
            }
        }
        /* renormalise */
        {
            float sum = 0.0f;
            for (i = 0; i < k; i++) sum += s->cands[i].v;
            if (sum > 0.0f) {
                for (i = 0; i < k; i++) s->cands[i].v /= sum;
            }
        }
    }

    /* ---- sample ---- */
    {
        float r = rng_float(s);
        float cum = 0.0f;
        for (i = 0; i < k; i++) {
            cum += s->cands[i].v;
            if (r < cum) return s->cands[i].id;
        }
        return s->cands[k - 1].id;
    }
}
