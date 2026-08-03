/* tokenizer.c -- GPT-2 style byte-level BPE tokeniser for Qwen3.5.
 *
 * Three stages, matching llama.cpp's llm_tokenizer_bpe:
 *
 *   1. special-token scan -- <|im_start|> and friends are matched
 *      literally and never split;
 *   2. pre-tokenisation -- the input is cut into words using the
 *      qwen35 regex, hand-compiled below (see split_words);
 *   3. byte-level BPE -- each word is mapped through the GPT-2 byte
 *      alphabet, then merged greedily by merge rank.
 *
 * The merge table and vocabulary come straight out of the GGUF file, so
 * the tokenisation is identical to the reference implementation.
 *
 * ANSI C (C89). No regex library: the pre-split pattern
 *
 *   (?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])
 *   |[^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+
 *   |\p{N}
 *   | ?[^\s\p{L}\p{M}\p{N}]+[\r\n]*
 *   |\s*[\r\n]+
 *   |\s+(?!\S)
 *   |\s+
 *
 * is implemented directly as a small hand-written scanner.
 */

#include "infer.h"
#include "unicode_tbl.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Unicode helpers                                                     */
/* ------------------------------------------------------------------ */

static int in_tbl(const unsigned long tbl[][2], int n, unsigned long cp) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < tbl[mid][0])      hi = mid - 1;
        else if (cp > tbl[mid][1]) lo = mid + 1;
        else                       return 1;
    }
    return 0;
}

static int uc_is_letter(unsigned long cp) {
    return in_tbl(uc_l_tbl, UC_L_COUNT, cp);
}
static int uc_is_mark(unsigned long cp) {
    return in_tbl(uc_m_tbl, UC_M_COUNT, cp);
}
static int uc_is_number(unsigned long cp) {
    return in_tbl(uc_n_tbl, UC_N_COUNT, cp);
}
static int uc_is_space(unsigned long cp) {
    /* \s in the Unicode sense, as used by the reference tokeniser. */
    if (cp == 0x20 || (cp >= 0x09 && cp <= 0x0D)) return 1;
    if (cp == 0x85 || cp == 0xA0 || cp == 0x1680) return 1;
    if (cp >= 0x2000 && cp <= 0x200A) return 1;
    if (cp == 0x2028 || cp == 0x2029) return 1;
    if (cp == 0x202F || cp == 0x205F || cp == 0x3000) return 1;
    return 0;
}

/* Decode one UTF-8 sequence. Returns bytes consumed (>=1) and stores the
 * codepoint. Invalid bytes decode as themselves (Latin-1 fallback), which
 * keeps the tokeniser total on arbitrary input. */
static int utf8_next(const unsigned char *s, size_t len, unsigned long *cp) {
    unsigned char c = s[0];

    if (c < 0x80) { *cp = c; return 1; }

    if ((c & 0xE0) == 0xC0 && len >= 2 && (s[1] & 0xC0) == 0x80) {
        *cp = ((unsigned long) (c & 0x1F) << 6) | (s[1] & 0x3F);
        if (*cp >= 0x80) return 2;
    } else if ((c & 0xF0) == 0xE0 && len >= 3 &&
               (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((unsigned long) (c & 0x0F) << 12)
            | ((unsigned long) (s[1] & 0x3F) << 6)
            | (s[2] & 0x3F);
        if (*cp >= 0x800) return 3;
    } else if ((c & 0xF8) == 0xF0 && len >= 4 &&
               (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80) {
        *cp = ((unsigned long) (c & 0x07) << 18)
            | ((unsigned long) (s[1] & 0x3F) << 12)
            | ((unsigned long) (s[2] & 0x3F) << 6)
            | (s[3] & 0x3F);
        if (*cp >= 0x10000) return 4;
    }

    *cp = c;
    return 1;
}

/* ------------------------------------------------------------------ */
/* GPT-2 byte alphabet                                                 */
/* ------------------------------------------------------------------ */

/* Byte b is displayed as codepoint byte_to_uc[b]; the vocabulary stores
 * tokens in that alphabet (hence the 'Ġ' for space). */
static unsigned short byte_to_uc[256];
static short          uc_to_byte_lo[512];   /* codepoints < 512 only */
static int            bt_ready = 0;

static void build_byte_table(void) {
    int b, n = 0;
    int i;

    if (bt_ready) return;

    for (i = 0; i < 512; i++) uc_to_byte_lo[i] = -1;

    for (b = 0; b < 256; b++) {
        int printable = (b >= '!' && b <= '~') ||
                        (b >= 0xA1 && b <= 0xAC) ||
                        (b >= 0xAE && b <= 0xFF);
        if (printable) {
            byte_to_uc[b] = (unsigned short) b;
        } else {
            byte_to_uc[b] = (unsigned short) (256 + n);
            n++;
        }
        uc_to_byte_lo[byte_to_uc[b]] = (short) b;
    }
    bt_ready = 1;
}

/* ------------------------------------------------------------------ */
/* Hash map: token text -> id                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *key;   /* not owned; points into vocab strings */
    int         val;
} hentry;

typedef struct {
    hentry *e;
    int     cap;       /* power of two */
    int     used;
} hmap;

static unsigned long hash_str(const char *s) {
    unsigned long h = 2166136261UL;
    while (*s) {
        h ^= (unsigned char) *s++;
        h *= 16777619UL;
        h &= 0xFFFFFFFFUL;
    }
    return h;
}

static void hm_init(hmap *m, int want) {
    int cap = 16;
    while (cap < want * 2) cap <<= 1;
    m->e    = (hentry *) xcalloc((size_t) cap, sizeof(hentry));
    m->cap  = cap;
    m->used = 0;
}

static void hm_put(hmap *m, const char *key, int val) {
    unsigned long h = hash_str(key) & (unsigned long) (m->cap - 1);
    while (m->e[h].key) {
        if (strcmp(m->e[h].key, key) == 0) { m->e[h].val = val; return; }
        h = (h + 1) & (unsigned long) (m->cap - 1);
    }
    m->e[h].key = key;
    m->e[h].val = val;
    m->used++;
}

static int hm_get(const hmap *m, const char *key) {
    unsigned long h = hash_str(key) & (unsigned long) (m->cap - 1);
    while (m->e[h].key) {
        if (strcmp(m->e[h].key, key) == 0) return m->e[h].val;
        h = (h + 1) & (unsigned long) (m->cap - 1);
    }
    return -1;
}

static void hm_free(hmap *m) {
    if (m->e) free(m->e);
    m->e = NULL;
}

/* ------------------------------------------------------------------ */
/* Tokenizer object                                                    */
/* ------------------------------------------------------------------ */

struct tokenizer {
    char **vocab;        /* n_vocab strings, owned by the gguf_file */
    int   *ttype;        /* token type per id */
    int    n_vocab;
    int    eos;

    hmap   tok2id;
    hmap   merge2rank;   /* "left right" -> rank */
    char  *merge_keys;   /* packed "left right\0" blobs */

    /* ids of special tokens, sorted longest-first for greedy matching */
    int   *spec_id;
    int    n_spec;

    strbuf piece;        /* scratch for tok_piece */
};

/* --- special tokens -------------------------------------------------- */

static void collect_specials(tokenizer *t) {
    int i, n = 0;

    for (i = 0; i < t->n_vocab; i++) {
        /* GGUF token types: 1 normal, 2 unknown, 3 control, 4 user-defined,
         * 5 unused, 6 byte. Control and user-defined tokens are matched
         * literally in the input when special handling is enabled. */
        if (t->ttype[i] == 3 || t->ttype[i] == 4) n++;
    }
    t->spec_id = (int *) xmalloc(sizeof(int) * (size_t) (n > 0 ? n : 1));
    t->n_spec  = 0;
    for (i = 0; i < t->n_vocab; i++) {
        if (t->ttype[i] == 3 || t->ttype[i] == 4) {
            t->spec_id[t->n_spec++] = i;
        }
    }
    /* insertion sort by descending text length: longest match wins */
    for (i = 1; i < t->n_spec; i++) {
        int id = t->spec_id[i];
        size_t li = strlen(t->vocab[id]);
        int j = i - 1;
        while (j >= 0 && strlen(t->vocab[t->spec_id[j]]) < li) {
            t->spec_id[j + 1] = t->spec_id[j];
            j--;
        }
        t->spec_id[j + 1] = id;
    }
}

tokenizer *tok_create(gguf_file *g) {
    tokenizer *t;
    const gg_kv *kv_tok, *kv_type, *kv_merge;
    long i;
    char *mp;

    kv_tok = gguf_find(g, "tokenizer.ggml.tokens");
    if (!kv_tok || kv_tok->type != GGUF_TYPE_ARRAY ||
        kv_tok->arr_type != GGUF_TYPE_STRING) {
        inf_log("model has no tokenizer.ggml.tokens array");
        return NULL;
    }

    t = (tokenizer *) xcalloc(1, sizeof(tokenizer));
    t->vocab   = (char **) kv_tok->arr;
    t->n_vocab = (int) kv_tok->arr_n;

    t->ttype = (int *) xcalloc((size_t) t->n_vocab, sizeof(int));
    kv_type  = gguf_find(g, "tokenizer.ggml.token_type");
    if (kv_type && kv_type->type == GGUF_TYPE_ARRAY &&
        kv_type->arr_type != GGUF_TYPE_STRING) {
        const float *v = (const float *) kv_type->arr;
        long n = kv_type->arr_n < t->n_vocab ? kv_type->arr_n : t->n_vocab;
        for (i = 0; i < n; i++) t->ttype[i] = (int) v[i];
    } else {
        for (i = 0; i < t->n_vocab; i++) t->ttype[i] = 1;
    }

    t->eos = (int) gguf_num(g, "tokenizer.ggml.eos_token_id", -1.0);

    build_byte_table();

    /* token text -> id */
    hm_init(&t->tok2id, t->n_vocab);
    for (i = 0; i < t->n_vocab; i++) {
        if (t->vocab[i]) hm_put(&t->tok2id, t->vocab[i], (int) i);
    }

    /* merges: "left right" -> rank */
    kv_merge = gguf_find(g, "tokenizer.ggml.merges");
    if (kv_merge && kv_merge->type == GGUF_TYPE_ARRAY &&
        kv_merge->arr_type == GGUF_TYPE_STRING) {
        char **m = (char **) kv_merge->arr;
        size_t total = 0;
        for (i = 0; i < kv_merge->arr_n; i++) total += strlen(m[i]) + 1;

        t->merge_keys = (char *) xmalloc(total);
        hm_init(&t->merge2rank, (int) kv_merge->arr_n);

        mp = t->merge_keys;
        for (i = 0; i < kv_merge->arr_n; i++) {
            size_t n = strlen(m[i]) + 1;
            memcpy(mp, m[i], n);
            hm_put(&t->merge2rank, mp, (int) i);
            mp += n;
        }
    } else {
        hm_init(&t->merge2rank, 16);
    }

    collect_specials(t);
    sb_init(&t->piece);
    return t;
}

void tok_free(tokenizer *t) {
    if (!t) return;
    hm_free(&t->tok2id);
    hm_free(&t->merge2rank);
    if (t->merge_keys) free(t->merge_keys);
    if (t->ttype)      free(t->ttype);
    if (t->spec_id)    free(t->spec_id);
    sb_free(&t->piece);
    free(t);
}

int tok_n_vocab(tokenizer *t) { return t->n_vocab; }
int tok_eos(tokenizer *t)     { return t->eos; }

int tok_is_control(tokenizer *t, int id) {
    if (id < 0 || id >= t->n_vocab) return 0;
    return t->ttype[id] == 3 || t->ttype[id] == 4;
}

int tok_id(tokenizer *t, const char *text) {
    return hm_get(&t->tok2id, text);
}

const char *tok_piece(tokenizer *t, int id, int *len) {
    const char *s;
    size_t i, n;

    sb_clear(&t->piece);
    if (id < 0 || id >= t->n_vocab || !t->vocab[id]) {
        if (len) *len = 0;
        return "";
    }
    s = t->vocab[id];
    n = strlen(s);

    /* Undo the GPT-2 byte alphabet: each codepoint maps back to one byte. */
    i = 0;
    while (i < n) {
        unsigned long cp;
        int adv = utf8_next((const unsigned char *) s + i, n - i, &cp);
        i += (size_t) adv;
        if (cp < 512 && uc_to_byte_lo[cp] >= 0) {
            char b = (char) (unsigned char) uc_to_byte_lo[cp];
            sb_add(&t->piece, &b, 1);
        } else {
            /* Not part of the byte alphabet (shouldn't happen for a
             * well-formed GPT-2 vocab): emit the UTF-8 verbatim. */
            sb_add(&t->piece, s + i - adv, (size_t) adv);
        }
    }

    if (len) *len = (int) t->piece.len;
    return t->piece.data;
}

/* ------------------------------------------------------------------ */
/* Stage 2: pre-tokenisation (the qwen35 regex, hand-compiled)         */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t off;
    size_t len;
} word;

typedef struct {
    word  *w;
    int    n;
    int    cap;
} wordlist;

static void wl_add(wordlist *wl, size_t off, size_t len) {
    if (len == 0) return;
    if (wl->n == wl->cap) {
        wl->cap = wl->cap ? wl->cap * 2 : 64;
        wl->w = (word *) xrealloc(wl->w, sizeof(word) * (size_t) wl->cap);
    }
    wl->w[wl->n].off = off;
    wl->w[wl->n].len = len;
    wl->n++;
}

/* Split [s, s+n) into pre-tokens following the qwen35 pattern. */
static void split_words(const char *s, size_t n, wordlist *wl) {
    const unsigned char *u = (const unsigned char *) s;
    size_t i = 0;

    while (i < n) {
        unsigned long cp;
        int adv = utf8_next(u + i, n - i, &cp);
        size_t start = i;

        /* --- alternative 1: English contractions ------------------- */
        if (cp == '\'' && i + 1 < n) {
            unsigned char c1 = u[i + 1];
            int m = 0;
            if (c1 == 's' || c1 == 'S' || c1 == 't' || c1 == 'T' ||
                c1 == 'm' || c1 == 'M' || c1 == 'd' || c1 == 'D') {
                m = 2;
            } else if (i + 2 < n) {
                unsigned char c2 = u[i + 2];
                if ((c1 == 'r' || c1 == 'R') && (c2 == 'e' || c2 == 'E')) m = 3;
                else if ((c1 == 'v' || c1 == 'V') && (c2 == 'e' || c2 == 'E')) m = 3;
                else if ((c1 == 'l' || c1 == 'L') && (c2 == 'l' || c2 == 'L')) m = 3;
            }
            if (m) {
                wl_add(wl, start, (size_t) m);
                i += (size_t) m;
                continue;
            }
        }

        /* --- alternative 2: [^\r\n\p{L}\p{N}]? [\p{L}\p{M}]+ ------- */
        {
            size_t j = i;
            int adv2 = adv;
            unsigned long cp2 = cp;
            int took_prefix = 0;

            if (cp != '\r' && cp != '\n' &&
                !uc_is_letter(cp) && !uc_is_number(cp)) {
                /* optional single leading non-letter/non-digit */
                size_t k = j + (size_t) adv2;
                if (k < n) {
                    unsigned long cpk;
                    int advk = utf8_next(u + k, n - k, &cpk);
                    if (uc_is_letter(cpk) || uc_is_mark(cpk)) {
                        took_prefix = 1;
                        j   = k;
                        cp2 = cpk;
                        adv2 = advk;
                    }
                }
                if (!took_prefix) goto alt3;
            } else if (!uc_is_letter(cp) && !uc_is_mark(cp)) {
                goto alt3;
            }

            /* one or more letters/marks */
            if (uc_is_letter(cp2) || uc_is_mark(cp2)) {
                j += (size_t) adv2;
                while (j < n) {
                    unsigned long cpj;
                    int advj = utf8_next(u + j, n - j, &cpj);
                    if (!uc_is_letter(cpj) && !uc_is_mark(cpj)) break;
                    j += (size_t) advj;
                }
                wl_add(wl, start, j - start);
                i = j;
                continue;
            }
        }

    alt3:
        /* --- alternative 3: \p{N} (a single digit) ----------------- */
        if (uc_is_number(cp)) {
            wl_add(wl, start, (size_t) adv);
            i += (size_t) adv;
            continue;
        }

        /* --- alternative 4:  ?[^\s\p{L}\p{M}\p{N}]+[\r\n]* --------- */
        {
            size_t j = i;
            unsigned long cpj = cp;
            int advj = adv;

            if (cp == ' ' && i + 1 < n) {
                unsigned long cpn;
                int advn = utf8_next(u + i + 1, n - i - 1, &cpn);
                if (!uc_is_space(cpn) && !uc_is_letter(cpn) &&
                    !uc_is_mark(cpn) && !uc_is_number(cpn)) {
                    j    = i + 1;
                    cpj  = cpn;
                    advj = advn;
                } 
            }

            if (!uc_is_space(cpj) && !uc_is_letter(cpj) &&
                !uc_is_mark(cpj) && !uc_is_number(cpj)) {
                j += (size_t) advj;
                while (j < n) {
                    unsigned long c2;
                    int a2 = utf8_next(u + j, n - j, &c2);
                    if (uc_is_space(c2) || uc_is_letter(c2) ||
                        uc_is_mark(c2) || uc_is_number(c2)) break;
                    j += (size_t) a2;
                }
                /* trailing [\r\n]* */
                while (j < n && (u[j] == '\r' || u[j] == '\n')) j++;
                wl_add(wl, start, j - start);
                i = j;
                continue;
            }
        }

        /* --- alternatives 5-7: whitespace runs --------------------- */
        if (uc_is_space(cp)) {
            /* \s*[\r\n]+ : the longest run of whitespace ending in a
             * newline run */
            size_t j = i, last_nl_end = 0;
            int seen_nl = 0;

            while (j < n) {
                unsigned long c2;
                int a2 = utf8_next(u + j, n - j, &c2);
                if (!uc_is_space(c2)) break;
                j += (size_t) a2;
                if (c2 == '\r' || c2 == '\n') {
                    seen_nl = 1;
                    last_nl_end = j;
                } else if (seen_nl) {
                    break;   /* newline run ended */
                }
            }
            if (seen_nl) {
                wl_add(wl, start, last_nl_end - start);
                i = last_nl_end;
                continue;
            }

            /* \s+(?!\S) : all but the last space if more text follows;
             * otherwise \s+ takes everything */
            {
                size_t j2 = i, prev = i;
                while (j2 < n) {
                    unsigned long c2;
                    int a2 = utf8_next(u + j2, n - j2, &c2);
                    if (!uc_is_space(c2)) break;
                    prev = j2;
                    j2 += (size_t) a2;
                }
                if (j2 < n && prev > i) {
                    /* leave the final space to attach to the next word */
                    wl_add(wl, start, prev - start);
                    i = prev;
                } else {
                    wl_add(wl, start, j2 - start);
                    i = j2;
                }
                continue;
            }
        }

        /* fallback: consume one codepoint so we always make progress */
        wl_add(wl, start, (size_t) adv);
        i += (size_t) adv;
    }
}

/* ------------------------------------------------------------------ */
/* Stage 3: byte-level BPE over one pre-token                          */
/* ------------------------------------------------------------------ */

/* A symbol is a slice of the byte-alphabet string, held in a doubly
 * linked list so merges are O(1). */
typedef struct {
    int   prev;
    int   next;
    size_t off;
    size_t len;
} bsym;

static void bpe_word(tokenizer *t, const char *w, size_t wlen,
                     int *out, int *nout, int max) {
    /* Map the raw bytes to the GPT-2 alphabet (UTF-8 of byte_to_uc). */
    static char  buf[8192];
    static bsym  sym[4096];
    static int   symoff[4096];
    size_t blen = 0;
    int nsym = 0;
    int i, head;
    char key[512];

    for (i = 0; i < (int) wlen; i++) {
        unsigned short cp = byte_to_uc[(unsigned char) w[i]];
        size_t start = blen;
        if (blen + 4 >= sizeof(buf)) break;
        if (cp < 0x80) {
            buf[blen++] = (char) cp;
        } else if (cp < 0x800) {
            buf[blen++] = (char) (0xC0 | (cp >> 6));
            buf[blen++] = (char) (0x80 | (cp & 0x3F));
        } else {
            buf[blen++] = (char) (0xE0 | (cp >> 12));
            buf[blen++] = (char) (0x80 | ((cp >> 6) & 0x3F));
            buf[blen++] = (char) (0x80 | (cp & 0x3F));
        }
        if (nsym >= (int) (sizeof(sym) / sizeof(sym[0]))) break;
        sym[nsym].off  = start;
        sym[nsym].len  = blen - start;
        sym[nsym].prev = nsym - 1;
        sym[nsym].next = nsym + 1;
        symoff[nsym] = nsym;
        nsym++;
    }
    if (nsym == 0) return;
    sym[nsym - 1].next = -1;
    head = 0;

    /* Greedy lowest-rank merge, repeated until nothing merges.
     * O(n^2) worst case per word, but words are short (< 100 bytes). */
    for (;;) {
        int best_rank = -1, best_a = -1;
        int a = head;

        while (a >= 0 && sym[a].next >= 0) {
            int b = sym[a].next;
            size_t la = sym[a].len, lb = sym[b].len;
            if (la + lb + 2 < sizeof(key)) {
                memcpy(key, buf + sym[a].off, la);
                key[la] = ' ';
                memcpy(key + la + 1, buf + sym[b].off, lb);
                key[la + 1 + lb] = '\0';
                {
                    int r = hm_get(&t->merge2rank, key);
                    if (r >= 0 && (best_rank < 0 || r < best_rank)) {
                        best_rank = r;
                        best_a = a;
                    }
                }
            }
            a = sym[a].next;
        }

        if (best_a < 0) break;

        /* merge best_a with its successor */
        {
            int b = sym[best_a].next;
            sym[best_a].len += sym[b].len;
            sym[best_a].next = sym[b].next;
            if (sym[b].next >= 0) sym[sym[b].next].prev = best_a;
        }
    }

    /* emit */
    {
        int a = head;
        while (a >= 0) {
            int id;
            size_t l = sym[a].len;
            if (l + 1 < sizeof(key)) {
                memcpy(key, buf + sym[a].off, l);
                key[l] = '\0';
                id = hm_get(&t->tok2id, key);
                if (id >= 0) {
                    if (*nout < max) out[(*nout)++] = id;
                } else {
                    /* Unmergeable slice: fall back to single bytes, which
                     * always exist in a byte-level vocabulary. */
                    size_t k = 0;
                    while (k < l) {
                        unsigned long cp;
                        int adv = utf8_next((const unsigned char *) buf +
                                            sym[a].off + k, l - k, &cp);
                        char one[8];
                        memcpy(one, buf + sym[a].off + k, (size_t) adv);
                        one[adv] = '\0';
                        id = hm_get(&t->tok2id, one);
                        if (id >= 0 && *nout < max) out[(*nout)++] = id;
                        k += (size_t) adv;
                    }
                }
            }
            a = sym[a].next;
        }
    }
    (void) symoff;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

/* Tokenise a plain (special-token-free) span. */
static void encode_plain(tokenizer *t, const char *s, size_t n,
                         int *out, int *nout, int max) {
    wordlist wl;
    int i;

    wl.w = NULL;
    wl.n = wl.cap = 0;
    split_words(s, n, &wl);

    for (i = 0; i < wl.n; i++) {
        bpe_word(t, s + wl.w[i].off, wl.w[i].len, out, nout, max);
    }
    if (wl.w) free(wl.w);
}

int tok_encode(tokenizer *t, const char *text, int special,
               int *out, int max) {
    size_t n = strlen(text);
    size_t i = 0, plain_start = 0;
    int nout = 0;

    while (i < n) {
        int matched = -1;

        if (special && text[i] == '<') {
            int k;
            for (k = 0; k < t->n_spec; k++) {
                const char *st = t->vocab[t->spec_id[k]];
                size_t sl = strlen(st);
                if (sl <= n - i && memcmp(text + i, st, sl) == 0) {
                    matched = k;
                    break;
                }
            }
        }

        if (matched >= 0) {
            const char *st = t->vocab[t->spec_id[matched]];
            size_t sl = strlen(st);
            if (i > plain_start) {
                encode_plain(t, text + plain_start, i - plain_start,
                             out, &nout, max);
            }
            if (nout < max) out[nout++] = t->spec_id[matched];
            i += sl;
            plain_start = i;
        } else {
            i++;
        }
    }

    if (n > plain_start) {
        encode_plain(t, text + plain_start, n - plain_start, out, &nout, max);
    }
    return nout;
}
