/* gguf.c -- GGUF v2/v3 container reader.
 *
 * Parses the header, the key/value metadata block and the tensor
 * directory, then loads the tensor data blob into memory.
 *
 * ANSI C (C89). GGUF stores every multi-byte value little-endian; they
 * are all read byte-wise here, so the reader is correct on big-endian
 * hosts as well.
 */

#include "infer.h"
#include "sys.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Little-endian primitive reads (host byte order independent)         */
/* ------------------------------------------------------------------ */

static int rd_bytes(FILE *f, void *dst, size_t n) {
    return fread(dst, 1, n, f) == n ? 0 : -1;
}

static int rd_u8(FILE *f, unsigned char *v) {
    int c = fgetc(f);
    if (c == EOF) return -1;
    *v = (unsigned char) c;
    return 0;
}

static int rd_u16(FILE *f, unsigned short *v) {
    unsigned char b[2];
    if (rd_bytes(f, b, 2)) return -1;
    *v = (unsigned short) (b[0] | (b[1] << 8));
    return 0;
}

static int rd_u32(FILE *f, unsigned long *v) {
    unsigned char b[4];
    if (rd_bytes(f, b, 4)) return -1;
    *v = (unsigned long) b[0]
       | ((unsigned long) b[1] << 8)
       | ((unsigned long) b[2] << 16)
       | ((unsigned long) b[3] << 24);
    return 0;
}

/* 64-bit values are returned as double: exact for anything < 2^53. */
static int rd_u64(FILE *f, double *v) {
    unsigned long lo, hi;
    if (rd_u32(f, &lo)) return -1;
    if (rd_u32(f, &hi)) return -1;
    *v = (double) lo + (double) hi * 4294967296.0;
    return 0;
}

static int rd_i64(FILE *f, double *v) {
    unsigned long lo, hi;
    if (rd_u32(f, &lo)) return -1;
    if (rd_u32(f, &hi)) return -1;
    if (hi & 0x80000000UL) {
        /* negative: two's complement */
        double mag = (double) (~lo & 0xFFFFFFFFUL)
                   + (double) (~hi & 0xFFFFFFFFUL) * 4294967296.0 + 1.0;
        *v = -mag;
    } else {
        *v = (double) lo + (double) hi * 4294967296.0;
    }
    return 0;
}

static int rd_f32(FILE *f, double *v) {
    unsigned char b[4];
    if (rd_bytes(f, b, 4)) return -1;
    *v = (double) inf_rd_f32p(b);
    return 0;
}

static int rd_f64(FILE *f, double *v) {
    unsigned char b[8];
    if (rd_bytes(f, b, 8)) return -1;
    *v = inf_rd_f64p(b);
    return 0;
}

/* GGUF strings: u64 length + raw bytes (not NUL-terminated in file). */
static char *rd_str(FILE *f) {
    double dn;
    size_t n;
    char *s;

    if (rd_u64(f, &dn)) return NULL;
    if (dn < 0 || dn > 64.0 * 1024.0 * 1024.0) return NULL;
    n = (size_t) dn;
    s = (char *) xmalloc(n + 1);
    if (n && rd_bytes(f, s, n)) {
        free(s);
        return NULL;
    }
    s[n] = '\0';
    return s;
}

/* Read one scalar of the given GGUF type into a double / string. */
static int rd_scalar(FILE *f, int type, double *num, char **str) {
    unsigned char u8;
    unsigned short u16;
    unsigned long u32;

    *str = NULL;
    switch (type) {
        case GGUF_TYPE_UINT8:
        case GGUF_TYPE_BOOL:
            if (rd_u8(f, &u8)) return -1;
            *num = (double) u8;
            return 0;
        case GGUF_TYPE_INT8:
            if (rd_u8(f, &u8)) return -1;
            *num = (double) (signed char) u8;
            return 0;
        case GGUF_TYPE_UINT16:
            if (rd_u16(f, &u16)) return -1;
            *num = (double) u16;
            return 0;
        case GGUF_TYPE_INT16:
            if (rd_u16(f, &u16)) return -1;
            *num = (double) (short) u16;
            return 0;
        case GGUF_TYPE_UINT32:
            if (rd_u32(f, &u32)) return -1;
            *num = (double) u32;
            return 0;
        case GGUF_TYPE_INT32:
            if (rd_u32(f, &u32)) return -1;
            *num = (double) (long) (signed long) (int) u32;
            return 0;
        case GGUF_TYPE_FLOAT32:
            return rd_f32(f, num);
        case GGUF_TYPE_UINT64:
            return rd_u64(f, num);
        case GGUF_TYPE_INT64:
            return rd_i64(f, num);
        case GGUF_TYPE_FLOAT64:
            return rd_f64(f, num);
        case GGUF_TYPE_STRING:
            *str = rd_str(f);
            *num = 0.0;
            return *str ? 0 : -1;
        default:
            return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Type sizes                                                          */
/* ------------------------------------------------------------------ */

const char *gg_type_name(int type) {
    switch (type) {
        case GGML_TYPE_F32:  return "F32";
        case GGML_TYPE_F16:  return "F16";
        case GGML_TYPE_Q4_0: return "Q4_0";
        case GGML_TYPE_Q4_1: return "Q4_1";
        case GGML_TYPE_Q5_0: return "Q5_0";
        case GGML_TYPE_Q5_1: return "Q5_1";
        case GGML_TYPE_Q8_0: return "Q8_0";
        case GGML_TYPE_Q2_K: return "Q2_K";
        case GGML_TYPE_Q3_K: return "Q3_K";
        case GGML_TYPE_Q4_K: return "Q4_K";
        case GGML_TYPE_Q5_K: return "Q5_K";
        case GGML_TYPE_Q6_K: return "Q6_K";
        case GGML_TYPE_Q8_K: return "Q8_K";
        default:             return "?";
    }
}

/* Bytes occupied by `nelem` elements of `type`. */
long gg_type_size(int type, long nelem) {
    switch (type) {
        case GGML_TYPE_F32:  return nelem * 4;
        case GGML_TYPE_F16:  return nelem * 2;
        case GGML_TYPE_Q4_0: return nelem / 32 * 18;
        case GGML_TYPE_Q4_1: return nelem / 32 * 20;
        case GGML_TYPE_Q5_0: return nelem / 32 * 22;
        case GGML_TYPE_Q5_1: return nelem / 32 * 24;
        case GGML_TYPE_Q8_0: return nelem / 32 * 34;
        case GGML_TYPE_Q2_K: return nelem / QK_K * 84;
        case GGML_TYPE_Q3_K: return nelem / QK_K * 110;
        case GGML_TYPE_Q4_K: return nelem / QK_K * 144;
        case GGML_TYPE_Q5_K: return nelem / QK_K * 176;
        case GGML_TYPE_Q6_K: return nelem / QK_K * 210;
        default:             return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

const gg_kv *gguf_find(const gguf_file *g, const char *key) {
    int i;
    for (i = 0; i < g->n_kv; i++) {
        if (strcmp(g->kv[i].key, key) == 0) return &g->kv[i];
    }
    return NULL;
}

double gguf_num(const gguf_file *g, const char *key, double dflt) {
    const gg_kv *k = gguf_find(g, key);
    if (!k || k->type == GGUF_TYPE_STRING || k->type == GGUF_TYPE_ARRAY) {
        return dflt;
    }
    return k->num;
}

const char *gguf_str(const gguf_file *g, const char *key) {
    const gg_kv *k = gguf_find(g, key);
    if (!k || k->type != GGUF_TYPE_STRING) return NULL;
    return k->str;
}

gg_tensor *gguf_tensor(gguf_file *g, const char *name) {
    int i;
    for (i = 0; i < g->n_tensors; i++) {
        if (strcmp(g->tensors[i].name, name) == 0) return &g->tensors[i];
    }
    return NULL;
}

int gguf_open(gguf_file *g, const char *path) {
    FILE *f;
    unsigned char magic[4];
    unsigned long version;
    double d_ntensors, d_nkv;
    int i, j;
    gg_off pos, blob_end;
    size_t got;

    memset(g, 0, sizeof(*g));

    /* Cheap, once per process, and catches a build that guessed the
     * host's byte order wrongly -- which otherwise fails silently. */
    if (inf_check_byte_order() != 0) return -1;

    f = fopen(path, "rb");
    if (!f) {
        inf_log("cannot open '%s'", path);
        return -1;
    }
    g->fp = f;

    if (rd_bytes(f, magic, 4)) goto bad;
    if (memcmp(magic, "GGUF", 4) != 0) {
        inf_log("not a GGUF file (bad magic)");
        goto bad;
    }
    if (rd_u32(f, &version)) goto bad;
    if (version != 2 && version != 3) {
        inf_log("unsupported GGUF version %lu (need 2 or 3)", version);
        goto bad;
    }
    if (rd_u64(f, &d_ntensors)) goto bad;
    if (rd_u64(f, &d_nkv)) goto bad;

    if (d_ntensors < 0 || d_ntensors > 100000.0 ||
        d_nkv < 0 || d_nkv > 100000.0) {
        inf_log("implausible GGUF header counts");
        goto bad;
    }
    g->n_tensors = (int) d_ntensors;
    g->n_kv      = (int) d_nkv;

    /* ---- key/value metadata ---- */
    g->kv = (gg_kv *) xcalloc((size_t) g->n_kv, sizeof(gg_kv));
    for (i = 0; i < g->n_kv; i++) {
        gg_kv *kv = &g->kv[i];
        unsigned long t;

        kv->key = rd_str(f);
        if (!kv->key) goto bad;
        if (rd_u32(f, &t)) goto bad;
        kv->type = (int) t;

        if (kv->type == GGUF_TYPE_ARRAY) {
            unsigned long at;
            double dn;
            long n;

            if (rd_u32(f, &at)) goto bad;
            if (rd_u64(f, &dn)) goto bad;
            kv->arr_type = (int) at;
            if (dn < 0 || dn > 20000000.0) {
                inf_log("array '%s' too large", kv->key);
                goto bad;
            }
            n = (long) dn;
            kv->arr_n = n;

            if (kv->arr_type == GGUF_TYPE_STRING) {
                char **v = (char **) xcalloc((size_t) n, sizeof(char *));
                kv->arr = v;
                for (j = 0; j < n; j++) {
                    v[j] = rd_str(f);
                    if (!v[j]) goto bad;
                }
            } else {
                /* Numeric arrays are kept as float: token_type and
                 * rope_sections are small ints, and this halves the
                 * memory cost of the 248k-entry token_type array on a
                 * 32-bit host compared with double. */
                float *v = (float *) xcalloc((size_t) n, sizeof(float));
                kv->arr = v;
                for (j = 0; j < n; j++) {
                    double num;
                    char *s;
                    if (rd_scalar(f, kv->arr_type, &num, &s)) goto bad;
                    v[j] = (float) num;
                }
            }
        } else {
            if (rd_scalar(f, kv->type, &kv->num, &kv->str)) goto bad;
        }
    }

    /* ---- tensor directory ---- */
    g->tensors = (gg_tensor *) xcalloc((size_t) g->n_tensors,
                                       sizeof(gg_tensor));
    for (i = 0; i < g->n_tensors; i++) {
        gg_tensor *t = &g->tensors[i];
        char *nm;
        unsigned long nd, ty;
        long nelem = 1;

        nm = rd_str(f);
        if (!nm) goto bad;
        strncpy(t->name, nm, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        free(nm);

        if (rd_u32(f, &nd)) goto bad;
        if (nd > GG_MAX_DIMS) {
            inf_log("tensor '%s' has %lu dims", t->name, nd);
            goto bad;
        }
        t->n_dims = (int) nd;
        for (j = 0; j < GG_MAX_DIMS; j++) t->ne[j] = 1;
        for (j = 0; j < (int) nd; j++) {
            double dv;
            if (rd_u64(f, &dv)) goto bad;
            t->ne[j] = (long) dv;
            nelem *= t->ne[j];
        }
        if (rd_u32(f, &ty)) goto bad;
        t->type = (int) ty;
        if (rd_u64(f, &t->offset)) goto bad;

        t->nbytes = gg_type_size(t->type, nelem);
        if (t->nbytes < 0) {
            inf_log("tensor '%s': unsupported type %s",
                    t->name, gg_type_name(t->type));
            goto bad;
        }
    }

    /* ---- tensor data blob ---- */
    {
        unsigned long align = (unsigned long)
            gguf_num(g, "general.alignment", 32.0);
        long cur = ftell(f);
        if (cur < 0) goto bad;
        if (align == 0) align = 32;
        pos = (gg_off) (((unsigned long) cur + align - 1) / align * align);
        g->data_start = pos;
    }

    /* Total blob size = max(offset + nbytes) over all tensors. */
    blob_end = 0.0;
    for (i = 0; i < g->n_tensors; i++) {
        gg_off end = g->tensors[i].offset + (gg_off) g->tensors[i].nbytes;
        if (end > blob_end) blob_end = end;
    }
    g->blob_size = blob_end;

    if (blob_end > 1900.0 * 1024.0 * 1024.0) {
        inf_log("tensor blob is %.0f MB -- too large for a 32-bit address "
                "space", blob_end / 1048576.0);
        goto bad;
    }

    /* Preferred path: map the tensor data read-only. The weights are then
     * demand-paged, so a machine with less RAM than the model still runs
     * (slowly), and nothing is ever written to swap. */
    g->map = sys_map_file(path, g->data_start, blob_end);
    if (g->map) {
        sys_map_advise_random(g->map);
        g->blob = (unsigned char *) sys_map_data(g->map);
        g->mapped = 1;
    } else {
        /* Fallback: read it all into memory. */
        g->blob = (unsigned char *) malloc((size_t) blob_end);
        if (!g->blob) {
            inf_log("cannot allocate %.1f MB for tensor data",
                    blob_end / 1048576.0);
            goto bad;
        }
        g->mapped = 0;

        if (fseek(f, (long) g->data_start, SEEK_SET) != 0) goto bad;
        got = fread(g->blob, 1, (size_t) blob_end, f);
        if (got != (size_t) blob_end) {
            inf_log("short read of tensor data (%lu of %.0f bytes)",
                    (unsigned long) got, blob_end);
            goto bad;
        }
    }

    for (i = 0; i < g->n_tensors; i++) {
        g->tensors[i].data = g->blob + (size_t) g->tensors[i].offset;
    }

    /* The blob is resident; the file handle is no longer needed. */
    fclose(f);
    g->fp = NULL;
    return 0;

bad:
    gguf_close(g);
    return -1;
}

const float *gguf_f32data(gg_tensor *t) {
#if INFER_BIG_ENDIAN
    long n;
    if (!t) return NULL;
    if (t->swapped) return t->swapped;
    n = t->nbytes / 4;
    if (n <= 0) return NULL;
    t->swapped = (float *) xmalloc(sizeof(float) * (size_t) n);
    inf_rd_f32v(t->data, t->swapped, n);
    return t->swapped;
#else
    /* The common case: the mapping already holds host-order floats. */
    return (const float *) t->data;
#endif
}

void gguf_close(gguf_file *g) {
    int i, j;

    if (g->kv) {
        for (i = 0; i < g->n_kv; i++) {
            gg_kv *kv = &g->kv[i];
            if (kv->key) free(kv->key);
            if (kv->str) free(kv->str);
            if (kv->arr) {
                if (kv->type == GGUF_TYPE_ARRAY &&
                    kv->arr_type == GGUF_TYPE_STRING) {
                    char **v = (char **) kv->arr;
                    for (j = 0; j < kv->arr_n; j++) {
                        if (v[j]) free(v[j]);
                    }
                }
                free(kv->arr);
            }
        }
        free(g->kv);
        g->kv = NULL;
    }
    if (g->tensors) {
#if INFER_BIG_ENDIAN
        for (i = 0; i < g->n_tensors; i++) {
            if (g->tensors[i].swapped) free(g->tensors[i].swapped);
        }
#endif
        free(g->tensors);
        g->tensors = NULL;
    }
    if (g->map) {
        sys_map_close(g->map);
        g->map  = NULL;
        g->blob = NULL;
    } else if (g->blob) {
        free(g->blob);
        g->blob = NULL;
    }
    if (g->fp) {
        fclose(g->fp);
        g->fp = NULL;
    }
}
