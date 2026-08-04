/* t_cache.c -- prompt-prefix reuse must not change the answer.
 *
 * chat and serve re-render the whole conversation every turn, so turn
 * N+1's prompt is normally turn N's prompt plus more tokens.
 * qwen35_reuse_prefix() lets the engine skip the shared part.
 *
 * That is only safe if it is *exactly* equivalent to recomputing. This
 * is the test that says so: the same prompt, decoded fresh and decoded
 * incrementally, must produce bit-identical logits.
 *
 * The architecture makes this less obvious than it sounds. Only 6 of
 * the 24 layers have a position-indexed KV cache. The other 18 are
 * Gated DeltaNet, whose recurrent state has absorbed every token in
 * order and cannot be rewound -- so the reuse rule has to be
 * all-or-nothing, and this test checks the rejection cases too.
 *
 *     ./build/t_cache model.gguf
 *
 * ANSI C (C89).
 */

#include "../src/infer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;

static void ck(const char *what, int ok) {
    printf("%-56s %s\n", what, ok ? "ok" : "** FAIL **");
    if (!ok) fails++;
}

/* Largest absolute difference between two logit vectors. */
static double maxdiff(const float *a, const float *b, int n) {
    double m = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double d = fabs((double) a[i] - (double) b[i]);
        if (d > m) m = d;
    }
    return m;
}

static int argmax(const float *v, int n) {
    int i, best = 0;
    for (i = 1; i < n; i++) if (v[i] > v[best]) best = i;
    return best;
}

int main(int argc, char **argv) {
    qwen35_model *m;
    qwen35_ctx *c;
    qwen35_params qp;
    char err[256];
    int nv, i;
    float *fresh, *inc;
    /* An arbitrary but fixed token sequence. Values are small so they
     * are valid in any vocabulary this model family uses. */
    static int seq[12] = { 100, 200, 300, 400, 500, 600,
                           700, 800, 900, 1000, 1100, 1200 };
    int split = 7;   /* pretend the first 7 were a previous turn */

    if (argc < 2) {
        printf("usage: t_cache model.gguf\n");
        return 0;
    }

    m = qwen35_load(argv[1], err, sizeof(err));
    if (!m) {
        printf("cannot load model: %s\n", err[0] ? err : "unknown");
        return 1;
    }
    nv = qwen35_n_vocab(m);

    qp.n_ctx = 512;
    qp.n_threads = 1;
    c = qwen35_ctx_create(m, &qp);

    fresh = (float *) malloc(sizeof(float) * (size_t) nv);
    inc   = (float *) malloc(sizeof(float) * (size_t) nv);
    if (!fresh || !inc) { printf("oom\n"); return 1; }

    /* ---- 1. an empty context reuses nothing ---- */
    qwen35_reset(c);
    ck("empty context reuses nothing",
       qwen35_reuse_prefix(c, seq, 12) == 0);

    /* ---- 2. decode the whole sequence fresh, keep the logits ---- */
    qwen35_reset(c);
    {
        float *lg = NULL;
        for (i = 0; i < 12; i++) lg = qwen35_decode(c, seq[i], i, i == 11);
        memcpy(fresh, lg, sizeof(float) * (size_t) nv);
    }

    /* ---- 3. an identical prompt reuses all but the last token ----
     * One token must always be left to decode, because the caller needs
     * a forward pass to obtain logits.
     *
     * Re-ingest first: step 2 left the history in place, but calling
     * reuse_prefix() is itself what a caller does *before* decoding, so
     * set up the state deliberately rather than relying on the previous
     * step's leftovers. */
    qwen35_reset(c);
    for (i = 0; i < 12; i++) qwen35_decode(c, seq[i], i, 0);
    ck("identical prompt reuses n-1 tokens",
       qwen35_reuse_prefix(c, seq, 12) == 11);

    /* ---- 4. the incremental path: 7 tokens, then the other 5 ---- */
    qwen35_reset(c);
    {
        float *lg = NULL;
        int reused;

        /* "previous turn": ingest the first 7 */
        for (i = 0; i < split; i++) {
            lg = qwen35_decode(c, seq[i], i, i == split - 1);
        }

        /* "this turn": the same 7 plus 5 more */
        reused = qwen35_reuse_prefix(c, seq, 12);
        ck("extended prompt reuses the whole previous turn",
           reused == split);

        for (i = reused; i < 12; i++) {
            lg = qwen35_decode(c, seq[i], i, i == 11);
        }
        memcpy(inc, lg, sizeof(float) * (size_t) nv);
    }

    /* ---- 5. the point of the whole exercise ---- */
    {
        double d = maxdiff(fresh, inc, nv);
        printf("%-56s %.3e\n", "max |logit difference|", d);
        ck("incremental logits are bit-identical to a fresh decode",
           d == 0.0);
        ck("...and therefore pick the same token",
           argmax(fresh, nv) == argmax(inc, nv));
    }

    /* ---- 6. divergence must reset, not silently reuse ----
     * A recurrent state cannot be rewound, so a prompt that differs
     * anywhere inside the history is unusable. */
    {
        static int other[12];
        memcpy(other, seq, sizeof(other));
        other[3] = 4242;                    /* edit inside the history */

        qwen35_reset(c);
        for (i = 0; i < 12; i++) qwen35_decode(c, seq[i], i, 0);

        ck("edited history resets instead of reusing",
           qwen35_reuse_prefix(c, other, 12) == 0);
    }

    /* ---- 7. a shorter prompt cannot reuse a longer history ---- */
    {
        qwen35_reset(c);
        for (i = 0; i < 12; i++) qwen35_decode(c, seq[i], i, 0);
        ck("shorter prompt resets (history is not a prefix of it)",
           qwen35_reuse_prefix(c, seq, 5) == 0);
    }

    /* ---- 8. after a reset nothing is reused ---- */
    {
        qwen35_reset(c);
        ck("reset clears the reusable history",
           qwen35_reuse_prefix(c, seq, 12) == 0);
    }

    /* ---- 9. a second extension in the same context ----
     * Simulates a third chat turn: 7 -> 12 -> 12+n. */
    {
        float *lg = NULL;
        static int longer[16];
        int reused;

        memcpy(longer, seq, sizeof(seq));
        longer[12] = 1300; longer[13] = 1400;
        longer[14] = 1500; longer[15] = 1600;

        qwen35_reset(c);
        for (i = 0; i < 12; i++) qwen35_decode(c, seq[i], i, 0);

        reused = qwen35_reuse_prefix(c, longer, 16);
        ck("a further extension reuses the previous 12", reused == 12);

        for (i = reused; i < 16; i++) {
            lg = qwen35_decode(c, longer[i], i, i == 15);
        }

        /* and the same thing computed from scratch */
        qwen35_reset(c);
        for (i = 0; i < 16; i++) {
            lg = qwen35_decode(c, longer[i], i, i == 15);
        }
        memcpy(fresh, lg, sizeof(float) * (size_t) nv);

        /* recompute incrementally once more to compare against `fresh` */
        qwen35_reset(c);
        for (i = 0; i < 12; i++) qwen35_decode(c, longer[i], i, 0);
        reused = qwen35_reuse_prefix(c, longer, 16);
        for (i = reused; i < 16; i++) {
            lg = qwen35_decode(c, longer[i], i, i == 15);
        }
        memcpy(inc, lg, sizeof(float) * (size_t) nv);

        ck("two-stage extension still matches a fresh decode",
           maxdiff(fresh, inc, nv) == 0.0);
    }

    free(fresh);
    free(inc);
    qwen35_ctx_free(c);
    qwen35_free(m);

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall cache tests passed\n", fails);
    return fails ? 1 : 0;
}
