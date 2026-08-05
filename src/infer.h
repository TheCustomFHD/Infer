/* infer.h -- shared declarations for the infer GGUF server.
 *
 * ANSI C (C89). No third-party dependencies. Only the C standard library
 * and (in net_*.c) the host OS socket API are used.
 *
 * Target: i486, 32-bit. The code avoids 64-bit integer types entirely --
 * file offsets are carried as `double` where they may exceed the range
 * of a long, and reduced to fseek chunks when used.
 *
 * Byte order: GGUF is defined little-endian, and every value read from
 * the file goes through a byte-wise accessor, so the code runs on
 * big-endian hosts too. See "Byte order" below.
 */

#ifndef INFER_H
#define INFER_H

#include <stddef.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Portability                                                         */
/* ------------------------------------------------------------------ */

/* A 64-bit-capable byte offset. i486 has no reliable 64-bit int in C89,
 * so we use double: it holds integers exactly up to 2^53, far beyond
 * any GGUF file we can mmap/read on a 32-bit host. */
typedef double gg_off;

#ifndef INFER_VERSION
#define INFER_VERSION "1.21.0"
#endif

/* ------------------------------------------------------------------ */
/* Byte order                                                          */
/*                                                                     */
/* GGUF stores every multi-byte value little-endian. Rather than swap   */
/* whole buffers, all file-derived values are read one byte at a time   */
/* and reassembled with shifts -- correct on either host, and free on   */
/* a little-endian one because the compiler folds the shifts back into  */
/* a single load.                                                      */
/*                                                                     */
/* INFER_BIG_ENDIAN can be forced with -DINFER_BIG_ENDIAN=1 (or 0) if   */
/* the autodetection below does not know your compiler.                */
/* ------------------------------------------------------------------ */

#ifndef INFER_BIG_ENDIAN
   /* Compiler-specific spellings matter here. GCC and Clang define
    * __sparc__ and __BYTE_ORDER__; Sun Studio defines neither -- it uses
    * __sparc (one trailing underscore) and __sparcv9, and -Xc suppresses
    * the unprefixed `sparc`. Missing that made a Sun Studio build on an
    * UltraSPARC compile as little-endian and emit pure garbage, with no
    * diagnostic. Check every spelling, and verify at run time below. */
#  if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#    if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#      define INFER_BIG_ENDIAN 1
#    else
#      define INFER_BIG_ENDIAN 0
#    endif
#  elif defined(__BIG_ENDIAN__) || defined(_BIG_ENDIAN) || \
        defined(__ARMEB__) || defined(__THUMBEB__) || defined(__AARCH64EB__) || \
        defined(_MIPSEB) || defined(__MIPSEB) || defined(__MIPSEB__) || \
        defined(__s390__) || defined(__s390x__) || \
        defined(__sparc__) || defined(__sparc) || defined(sparc) || \
        defined(__sparcv8) || defined(__sparcv9) || defined(__sparc_v9__) || \
        defined(__hppa__) || defined(__hppa) || \
        defined(__m68k__) || defined(mc68000) || defined(__PPC__) || \
        defined(_ARCH_PPC) || defined(__powerpc__) || defined(__ppc__) || \
        defined(__BIG_ENDIAN) || defined(_M_PPC)
#    define INFER_BIG_ENDIAN 1
#  elif defined(__LITTLE_ENDIAN__) || defined(_LITTLE_ENDIAN) || \
        defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || \
        defined(_M_X64) || defined(__amd64__) || \
        defined(__ARMEL__) || defined(__AARCH64EL__) || \
        defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__) || \
        defined(__alpha__) || defined(__riscv)
#    define INFER_BIG_ENDIAN 0
#  else
     /* Unknown target. Refuse rather than guess: guessing wrong produces
      * a program that loads the model, reports correct metadata, runs at
      * full speed and emits nonsense. Build with -DINFER_BIG_ENDIAN=1
      * or =0 to say which this machine is. */
#    error "cannot determine byte order; build with -DINFER_BIG_ENDIAN=1 (big) or =0 (little)"
#  endif
#endif

/* Read a little-endian f32 from raw bytes.
 *
 * Defined here rather than in quant.c because gguf.c, quant.c and
 * qwen35.c all need it. The little-endian path is a plain memcpy, which
 * every compiler turns into one load; the big-endian path assembles the
 * four bytes explicitly. */
/* Verify at run time that INFER_BIG_ENDIAN matches the actual host, and
 * that the fp32/fp16 readers behave. Returns 0 on success; on failure
 * prints what is wrong and returns non-zero. Called by gguf_open(),
 * because a mismatch here produces plausible-looking garbage rather
 * than any kind of error. */
int inf_check_byte_order(void);

float inf_rd_f32p(const unsigned char *p);
double inf_rd_f64p(const unsigned char *p);

/* Copy `n` little-endian f32 values from `src` into host order.
 * On a little-endian host this is exactly memcpy. */
void inf_rd_f32v(const unsigned char *src, float *dst, long n);

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

void inf_log(const char *fmt, ...);
void inf_die(const char *fmt, ...);
extern int inf_verbose;

/* ------------------------------------------------------------------ */
/* GGUF quantisation types we understand                               */
/* ------------------------------------------------------------------ */

#define GGML_TYPE_F32   0
#define GGML_TYPE_F16   1
#define GGML_TYPE_Q4_0  2
#define GGML_TYPE_Q4_1  3
#define GGML_TYPE_Q5_0  6
#define GGML_TYPE_Q5_1  7
#define GGML_TYPE_Q8_0  8
#define GGML_TYPE_Q2_K 10
#define GGML_TYPE_Q3_K 11
#define GGML_TYPE_Q4_K 12
#define GGML_TYPE_Q5_K 13
#define GGML_TYPE_Q6_K 14
#define GGML_TYPE_Q8_K 15

#define QK_K 256

/* ------------------------------------------------------------------ */
/* Tensors                                                             */
/* ------------------------------------------------------------------ */

#define GG_MAX_DIMS 4

typedef struct {
    char          name[64];
    int           n_dims;
    long          ne[GG_MAX_DIMS];  /* element counts per dim, ne[0] fastest */
    int           type;             /* GGML_TYPE_* */
    gg_off        offset;           /* offset from start of tensor data blob */
    unsigned char *data;            /* resolved pointer into the model blob */
    long          nbytes;
    /* Big-endian hosts only: byte-swapped copy of an F32 tensor, made on
     * first use by gguf_f32data(). NULL and unused on little-endian. */
    float        *swapped;
} gg_tensor;

/* ------------------------------------------------------------------ */
/* GGUF key/value metadata                                             */
/* ------------------------------------------------------------------ */

#define GGUF_TYPE_UINT8    0
#define GGUF_TYPE_INT8     1
#define GGUF_TYPE_UINT16   2
#define GGUF_TYPE_INT16    3
#define GGUF_TYPE_UINT32   4
#define GGUF_TYPE_INT32    5
#define GGUF_TYPE_FLOAT32  6
#define GGUF_TYPE_BOOL     7
#define GGUF_TYPE_STRING   8
#define GGUF_TYPE_ARRAY    9
#define GGUF_TYPE_UINT64  10
#define GGUF_TYPE_INT64   11
#define GGUF_TYPE_FLOAT64 12

typedef struct {
    char   *key;
    int     type;      /* GGUF_TYPE_* */
    int     arr_type;  /* element type when type == ARRAY */
    long    arr_n;     /* element count when type == ARRAY */
    double  num;       /* scalar numeric value */
    char   *str;       /* scalar string value (owned) */
    void   *arr;       /* array payload (owned): char** for strings,
                          double* for numerics */
} gg_kv;

struct sys_map;   /* see sys.h */

typedef struct {
    FILE       *fp;
    gg_kv      *kv;
    int         n_kv;
    gg_tensor  *tensors;
    int         n_tensors;
    gg_off      data_start;      /* file offset of tensor data blob */
    unsigned char *blob;         /* tensor data: mapped, or read into RAM */
    gg_off      blob_size;
    struct sys_map *map;         /* non-NULL when the blob is mmap'd */
    int         mapped;
} gguf_file;

int          gguf_open(gguf_file *g, const char *path);
void         gguf_close(gguf_file *g);
const gg_kv *gguf_find(const gguf_file *g, const char *key);
double       gguf_num(const gguf_file *g, const char *key, double dflt);
const char  *gguf_str(const gguf_file *g, const char *key);
gg_tensor   *gguf_tensor(gguf_file *g, const char *name);

/* An F32 tensor's data in host byte order.
 *
 * On a little-endian host this is just (const float *) t->data -- the
 * mapping is used directly and nothing is copied. On a big-endian host
 * the blob is mapped read-only, so a swapped copy is made once, on
 * first use, and cached on the tensor.
 *
 * Only valid for GGML_TYPE_F32 tensors. */
const float *gguf_f32data(gg_tensor *t);
const char  *gg_type_name(int type);
long         gg_type_size(int type, long nelem);

/* ------------------------------------------------------------------ */
/* Dequantisation / linear algebra (quant.c)                           */
/* ------------------------------------------------------------------ */

/* Dequantise `n` elements of a quantised row into f32. */
void  q_dequant_row(int type, const unsigned char *src, float *dst, long n);

/* y = W * x, where W is a quantised matrix of `nrows` rows of `ncols`
 * elements each (row-major, ne[0]=ncols). x is f32[ncols]. */
void  q_matvec(int type, const unsigned char *w, const float *x,
               float *y, long ncols, long nrows);

float q_fp16_to_fp32(unsigned short h);

/* ------------------------------------------------------------------ */
/* Tokeniser (tokenizer.c)                                             */
/* ------------------------------------------------------------------ */

typedef struct tokenizer tokenizer;

tokenizer  *tok_create(gguf_file *g);
void        tok_free(tokenizer *t);
/* Tokenise UTF-8 `text`. Returns count, fills `out` up to `max`.
 * `special` enables matching of <|im_start|>-style control tokens. */
int         tok_encode(tokenizer *t, const char *text, int special,
                       int *out, int max);
/* Append the UTF-8 form of token `id` to a dynamically grown buffer. */
const char *tok_piece(tokenizer *t, int id, int *len);
int         tok_id(tokenizer *t, const char *token_text);
int         tok_n_vocab(tokenizer *t);
int         tok_eos(tokenizer *t);
int         tok_is_control(tokenizer *t, int id);

/* ------------------------------------------------------------------ */
/* Model / engine (qwen35.c)                                           */
/* ------------------------------------------------------------------ */

typedef struct qwen35_model qwen35_model;
typedef struct qwen35_ctx   qwen35_ctx;

typedef struct {
    int   n_ctx;      /* max attention context (tokens) */
    int   n_threads;  /* reserved; engine is single-threaded on i486 */
} qwen35_params;

qwen35_model *qwen35_load(const char *path, char *errbuf, size_t errlen);
void          qwen35_free(qwen35_model *m);
tokenizer    *qwen35_tokenizer(qwen35_model *m);
const char   *qwen35_name(qwen35_model *m);
int           qwen35_n_vocab(qwen35_model *m);
int           qwen35_n_ctx_train(qwen35_model *m);
/* The Jinja chat template from the GGUF, or NULL if absent. */
const char   *qwen35_chat_template(qwen35_model *m);

qwen35_ctx   *qwen35_ctx_create(qwen35_model *m, const qwen35_params *p);
void          qwen35_ctx_free(qwen35_ctx *c);
/* Reset all recurrent state and the KV cache (start a new sequence). */
void          qwen35_reset(qwen35_ctx *c);
/* Run one token through the network at absolute position `pos`.
 * Returns a pointer to the logits array (n_vocab floats), owned by ctx.
 * Pass want_logits=0 during prompt ingestion to skip the LM head. */
/* Length of the leading run of `toks` already present in the context,
 * which the caller may skip re-decoding. Resets the context and returns
 * 0 when the prompt is not an extension of what was ingested before.
 *
 * The skipped tokens still have to be handed to the sampler; only the
 * forward passes are avoided. */
int           qwen35_reuse_prefix(qwen35_ctx *c, const int *toks, int n);

float        *qwen35_decode(qwen35_ctx *c, int token, int pos, int want_logits);
int           qwen35_n_ctx(qwen35_ctx *c);

/* ------------------------------------------------------------------ */
/* Sampling (sampler.c)                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    float temperature;
    float top_p;
    int   top_k;
    float repeat_penalty;
    int   repeat_last_n;
    unsigned long seed;
} sampler_params;

typedef struct sampler sampler;

void      sampler_default(sampler_params *p);
sampler  *sampler_create(const sampler_params *p, int n_vocab);
void      sampler_free(sampler *s);
void      sampler_reset(sampler *s);
void      sampler_accept(sampler *s, int token);
int       sampler_pick(sampler *s, float *logits);

/* ------------------------------------------------------------------ */
/* Networking abstraction (net.h implements this; see net.h)           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Small helpers (util.c)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} strbuf;

void  sb_init(strbuf *b);
void  sb_free(strbuf *b);
void  sb_clear(strbuf *b);
int   sb_add(strbuf *b, const char *s, size_t n);
int   sb_puts(strbuf *b, const char *s);
int   sb_printf(strbuf *b, const char *fmt, ...);
/* Append `s` escaped as the body of a JSON string (no surrounding quotes). */
int   sb_json_escape(strbuf *b, const char *s, size_t n);

/* Read a whole text file (templates). Returns a malloc'd NUL-terminated
 * string with CRLF normalised to LF, or NULL with a reason in errbuf. */
char *load_text_file(const char *path, char *errbuf, size_t errlen);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

#endif /* INFER_H */
