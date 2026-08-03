/* t_quant.c -- dump dequantised leading values of selected tensors so
 * they can be diffed against a reference implementation. */

#include "../src/infer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *want[] = {
    "token_embd.weight",
    "blk.0.attn_qkv.weight",
    "blk.0.attn_gate.weight",
    "blk.0.ssm_alpha.weight",
    "blk.0.ffn_gate.weight",
    "blk.0.ffn_down.weight",
    "blk.3.attn_q.weight",
    "blk.3.attn_v.weight",
    "blk.23.ffn_down.weight",
    NULL
};

int main(int argc, char **argv) {
    gguf_file g;
    float *buf;
    int i, w;

    if (argc < 2) { fprintf(stderr, "usage: t_quant model.gguf\n"); return 2; }
    if (gguf_open(&g, argv[1])) return 1;

    buf = (float *) malloc(sizeof(float) * 4096);

    for (w = 0; want[w]; w++) {
        gg_tensor *t = gguf_tensor(&g, want[w]);
        long n;
        if (!t) { printf("%s MISSING\n", want[w]); continue; }
        n = t->ne[0] < 4096 ? t->ne[0] : 4096;
        q_dequant_row(t->type, t->data, buf, n);
        printf("%-26s %-5s", t->name, gg_type_name(t->type));
        for (i = 0; i < 8; i++) printf(" % .6f", buf[i]);
        printf("\n");
    }

    free(buf);
    gguf_close(&g);
    return 0;
}
