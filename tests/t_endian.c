/* t_endian.c -- byte-order neutrality.
 *
 * GGUF is little-endian by definition. Every multi-byte value read from
 * a model file must therefore be assembled byte-wise, so that the same
 * file produces the same numbers on a big-endian host.
 *
 * These checks are host-independent: they run on a little-endian
 * machine and on a big-endian one and must print the same thing. With a
 * model argument they additionally decode real tensors, which is how
 * the s390x comparison in FINDINGS.md was made.
 *
 * ANSI C (C89).
 */

#include "../src/infer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int fails = 0;

static void ck(const char *what, int ok) {
    printf("%-52s %s\n", what, ok ? "ok" : "** FAIL **");
    if (!ok) fails++;
}

static void ckf(const char *what, double got, double want) {
    int ok = fabs(got - want) < 1e-9;
    printf("%-52s %s", what, ok ? "ok" : "** FAIL **");
    if (!ok) printf("  (got %g, want %g)", got, want);
    printf("\n");
    if (!ok) fails++;
}

int main(int argc, char **argv) {
    printf("host is %s-endian (INFER_BIG_ENDIAN=%d)\n\n",
           INFER_BIG_ENDIAN ? "big" : "little", INFER_BIG_ENDIAN);

    /* ---- the detection macro must match reality ---- */
    {
        unsigned long probe = 1;
        int host_is_be = (*(unsigned char *) &probe) == 0;
        ck("INFER_BIG_ENDIAN agrees with a runtime probe",
           host_is_be == (INFER_BIG_ENDIAN ? 1 : 0));
    }

    /* ---- f32 from little-endian bytes ---- */
    {
        /* 1.0f = 0x3F800000 -> 00 00 80 3F little-endian */
        unsigned char one[4];
        unsigned char negtwo[4];
        one[0]=0x00; one[1]=0x00; one[2]=0x80; one[3]=0x3F;
        negtwo[0]=0x00; negtwo[1]=0x00; negtwo[2]=0x00; negtwo[3]=0xC0;
        ckf("inf_rd_f32p(00 00 80 3F) == 1.0",
            (double) inf_rd_f32p(one), 1.0);
        ckf("inf_rd_f32p(00 00 00 C0) == -2.0",
            (double) inf_rd_f32p(negtwo), -2.0);
    }

    /* ---- f64 from little-endian bytes ---- */
    {
        /* 1.5 = 0x3FF8000000000000 */
        unsigned char b[8];
        b[0]=0;b[1]=0;b[2]=0;b[3]=0;b[4]=0;b[5]=0;b[6]=0xF8;b[7]=0x3F;
        ckf("inf_rd_f64p(.. F8 3F) == 1.5", inf_rd_f64p(b), 1.5);
    }

    /* ---- vector copy ---- */
    {
        unsigned char src[8];
        float dst[2];
        src[0]=0x00; src[1]=0x00; src[2]=0x80; src[3]=0x3F;   /*  1.0 */
        src[4]=0x00; src[5]=0x00; src[6]=0x00; src[7]=0x40;   /*  2.0 */
        inf_rd_f32v(src, dst, 2);
        ckf("inf_rd_f32v element 0", (double) dst[0], 1.0);
        ckf("inf_rd_f32v element 1", (double) dst[1], 2.0);
    }

    /* ---- fp16 -> fp32 ----
     *
     * The regression this file exists for: q_fp16_to_fp32 built its
     * result in an `unsigned long` and memcpy'd 4 bytes out of it. On a
     * big-endian LP64 host that copies the high half -- every value
     * came back 0.0, so every quantisation scale in the model was zero
     * and generation produced garbage while the file still parsed. */
    ckf("fp16 0x3C00 ==  1.0", (double) q_fp16_to_fp32(0x3C00),  1.0);
    ckf("fp16 0xC000 == -2.0", (double) q_fp16_to_fp32(0xC000), -2.0);
    ckf("fp16 0x0000 ==  0.0", (double) q_fp16_to_fp32(0x0000),  0.0);
    ckf("fp16 0x3555 ~=  0.333252", (double) q_fp16_to_fp32(0x3555),
        0.333251953125);
    ck ("fp16 0x7C00 == +inf", q_fp16_to_fp32(0x7C00) > 3.0e38f);
    ckf("fp16 0x0001 == smallest subnormal",
        (double) q_fp16_to_fp32(0x0001), 5.9604644775390625e-08);

    /* ---- rd_f16p reads little-endian pairs ---- */
    {
        unsigned char h[2];
        h[0] = 0x00; h[1] = 0x3C;               /* 0x3C00 = 1.0 */
        ckf("f16 pair 00 3C == 1.0",
            (double) q_fp16_to_fp32((unsigned short) (h[0] | (h[1] << 8))),
            1.0);
    }

    /* ---- with a model: real tensors must decode identically ---- */
    if (argc > 1) {
        gguf_file g;
        printf("\nmodel: %s\n", argv[1]);
        if (gguf_open(&g, argv[1]) != 0) {
            printf("  cannot open -- skipping\n");
        } else {
            gg_tensor *t;
            float buf[512];
            printf("  rope_base %g  rms_eps %g\n",
                   gguf_num(&g, "qwen35.rope.freq_base", -1.0),
                   gguf_num(&g, "qwen35.attention.layer_norm_rms_epsilon",
                            -1.0));
            t = gguf_tensor(&g, "blk.0.attn_norm.weight");
            if (t && t->data) {
                const float *v = gguf_f32data(t);
                printf("  F32 attn_norm  %.6f %.6f %.6f %.6f\n",
                       v[0], v[1], v[2], v[3]);
            }
            t = gguf_tensor(&g, "blk.0.ffn_gate.weight");
            if (t && t->data) {
                q_dequant_row(t->type, t->data, buf, 8);
                printf("  %-14s %.6f %.6f %.6f %.6f\n", gg_type_name(t->type),
                       buf[0], buf[1], buf[2], buf[3]);
            }
            t = gguf_tensor(&g, "token_embd.weight");
            if (t && t->data) {
                q_dequant_row(t->type, t->data, buf, 8);
                printf("  %-14s %.6f %.6f %.6f %.6f\n", gg_type_name(t->type),
                       buf[0], buf[1], buf[2], buf[3]);
            }
            gguf_close(&g);
            printf("  (these lines must match byte-for-byte on every host)\n");
        }
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall endian tests passed\n", fails);
    return fails ? 1 : 0;
}
