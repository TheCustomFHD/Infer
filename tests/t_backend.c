/* t_backend.c -- compare every matvec backend against the reference.
 *
 * Runs each backend over real weight matrices from a model file and
 * reports both the numerical difference from the scalar reference and
 * the throughput.
 *
 *   ./build/t_backend model.gguf
 */

#include "../src/infer.h"
#include "../src/backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static const char *pick[] = {
    "blk.0.ffn_gate.weight",     /* Q4_K */
    "blk.0.attn_qkv.weight",     /* Q5_K */
    "blk.0.ffn_down.weight",     /* Q6_K */
    "blk.0.ssm_alpha.weight",    /* Q8_0 */
    NULL
};

int main(int argc, char **argv) {
    gguf_file g;
    float *x, *yref, *ytst;
    int k;

    if (argc < 2) { fprintf(stderr, "usage: t_backend model.gguf\n"); return 2; }
    if (gguf_open(&g, argv[1])) return 1;

    bk_print_list(stdout);
    printf("\n");

    /* deterministic pseudo-random activations */
    x = (float *) xmalloc(sizeof(float) * 8192);
    {
        unsigned long s = 12345;
        int i;
        for (i = 0; i < 8192; i++) {
            s = (s * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
            x[i] = ((float) (s % 20000) / 10000.0f) - 1.0f;
        }
    }

    yref = (float *) xmalloc(sizeof(float) * 8192);
    ytst = (float *) xmalloc(sizeof(float) * 8192);

    for (k = 0; pick[k]; k++) {
        gg_tensor *t = gguf_tensor(&g, pick[k]);
        long ncols, nrows;
        const char *names[4];
        int nb = 0, bi;

        if (!t) { printf("%s: missing\n", pick[k]); continue; }
        ncols = t->ne[0];
        nrows = t->ne[1] > 512 ? 512 : t->ne[1];

        printf("%-24s %-5s %ldx%ld\n", t->name, gg_type_name(t->type),
               nrows, ncols);

        bk_select("ref");
        q_matvec(t->type, t->data, x, yref, ncols, nrows);

        names[nb++] = "ref";
        names[nb++] = "i8";
#ifdef INFER_HAVE_MMX
        names[nb++] = "mmx";
#endif
#ifdef INFER_HAVE_VIS
        names[nb++] = "vis";
#endif

        for (bi = 0; bi < nb; bi++) {
            clock_t t0, t1;
            double secs;
            int reps = 0;
            double maxrel = 0.0, l2n = 0.0, l2d = 0.0;
            long r;

            if (bk_select(names[bi]) != 0) {
                printf("   %-4s unavailable\n", names[bi]);
                continue;
            }

            q_matvec(t->type, t->data, x, ytst, ncols, nrows);

            for (r = 0; r < nrows; r++) {
                double d = (double) ytst[r] - (double) yref[r];
                double a = fabs((double) yref[r]);
                l2n += d * d;
                l2d += (double) yref[r] * (double) yref[r];
                if (a > 1e-4) {
                    double rel = fabs(d) / a;
                    if (rel > maxrel) maxrel = rel;
                }
            }

            t0 = clock();
            do {
                q_matvec(t->type, t->data, x, ytst, ncols, nrows);
                reps++;
                t1 = clock();
            } while ((double) (t1 - t0) / CLOCKS_PER_SEC < 0.35);
            secs = (double) (t1 - t0) / CLOCKS_PER_SEC;

            printf("   %-4s  %8.1f Mflop/s   rel-l2 %.3e   max-rel %.3e\n",
                   names[bi],
                   (double) reps * nrows * ncols * 2.0 / secs / 1e6,
                   l2d > 0.0 ? sqrt(l2n / l2d) : 0.0,
                   maxrel);
        }
        printf("\n");
    }

    free(x); free(yref); free(ytst);
    bk_free_scratch();
    gguf_close(&g);
    return 0;
}
