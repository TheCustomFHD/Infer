/* t_tokenizer.c -- round-trip and known-answer tests for the tokeniser. */
#include "../src/infer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

static void check_roundtrip(tokenizer *t, const char *s) {
    int toks[4096], n, i;
    strbuf b;
    sb_init(&b);
    n = tok_encode(t, s, 1, toks, 4096);
    for (i = 0; i < n; i++) {
        int l; const char *p = tok_piece(t, toks[i], &l);
        sb_add(&b, p, (size_t) l);
    }
    if (strcmp(b.data, s) != 0) {
        printf("  FAIL roundtrip: %s\n    got: %s\n", s, b.data);
        fails++;
    } else {
        printf("  ok  (%2d tok) %s\n", n, s);
    }
    sb_free(&b);
}

int main(int argc, char **argv) {
    gguf_file g; tokenizer *t;
    if (argc < 2) { fprintf(stderr, "usage: t_tokenizer model.gguf\n"); return 2; }
    if (gguf_open(&g, argv[1])) return 1;
    t = tok_create(&g);
    if (!t) return 1;

    printf("round-trip tests:\n");
    check_roundtrip(t, "Hello, world!");
    check_roundtrip(t, "The capital of France is Paris.");
    check_roundtrip(t, "  leading and trailing spaces  ");
    check_roundtrip(t, "tabs\tand\nnewlines\r\nhere");
    check_roundtrip(t, "numbers 1234567890 and 3.14159");
    check_roundtrip(t, "don't can't won't I'll we've they're");
    check_roundtrip(t, "DON'T CAN'T WON'T");
    check_roundtrip(t, "CJK: \344\275\240\345\245\275\344\270\226\347\225\214");
    check_roundtrip(t, "emoji: \360\237\214\215\360\237\232\200");
    check_roundtrip(t, "accents: caf\303\251 na\303\257ve \303\274ber");
    check_roundtrip(t, "punct!!! ???  ...---===");
    check_roundtrip(t, "code: int x = f(a[i], &b->c);");
    check_roundtrip(t, "mixed   \t \n  whitespace\n\n\nruns");
    check_roundtrip(t, "<|im_start|>user\nhi<|im_end|>\n");

    printf("\nspecial tokens:\n");
    {
        int toks[64], n, i;
        n = tok_encode(t, "<|im_start|>user\nhi<|im_end|>\n", 1, toks, 64);
        printf("  ids:");
        for (i = 0; i < n; i++) printf(" %d", toks[i]);
        printf("\n");
        if (toks[0] != tok_id(t, "<|im_start|>")) {
            printf("  FAIL: first token is not <|im_start|>\n"); fails++;
        } else printf("  ok  <|im_start|> matched as one token\n");
    }

    printf("\n%s\n", fails ? "FAILURES" : "all tokenizer tests passed");
    tok_free(t); gguf_close(&g);
    return fails ? 1 : 0;
}
