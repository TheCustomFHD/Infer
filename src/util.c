/* util.c -- allocation helpers, growable string buffer, logging.
 * ANSI C (C89). */

#include "infer.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

int inf_verbose = 0;

void inf_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void inf_die(const char *fmt, ...) {
    va_list ap;
    fputs("infer: fatal: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Little-endian scalar readers                                        */
/*                                                                     */
/* GGUF is little-endian by definition. On a little-endian host these   */
/* are a straight memcpy, which compilers fold into a single load, so   */
/* the portability costs nothing measurable. On a big-endian host the   */
/* bytes are reassembled explicitly.                                   */
/*                                                                     */
/* memcpy rather than a pointer cast: the blob is only byte-aligned,    */
/* and a cast would be both an alignment fault risk (SPARC, older ARM)  */
/* and a strict-aliasing violation.                                    */
/* ------------------------------------------------------------------ */

float inf_rd_f32p(const unsigned char *p) {
    float out;
#if INFER_BIG_ENDIAN
    unsigned char b[4];
    b[0] = p[3]; b[1] = p[2]; b[2] = p[1]; b[3] = p[0];
    memcpy(&out, b, 4);
#else
    memcpy(&out, p, 4);
#endif
    return out;
}

double inf_rd_f64p(const unsigned char *p) {
    double out;
#if INFER_BIG_ENDIAN
    unsigned char b[8];
    b[0] = p[7]; b[1] = p[6]; b[2] = p[5]; b[3] = p[4];
    b[4] = p[3]; b[5] = p[2]; b[6] = p[1]; b[7] = p[0];
    memcpy(&out, b, 8);
#else
    memcpy(&out, p, 8);
#endif
    return out;
}

void inf_rd_f32v(const unsigned char *src, float *dst, long n) {
#if INFER_BIG_ENDIAN
    long i;
    for (i = 0; i < n; i++) dst[i] = inf_rd_f32p(src + i * 4);
#else
    memcpy(dst, src, (size_t) n * 4);
#endif
}

void *xmalloc(size_t n) {
    void *p;
    if (n == 0) n = 1;
    p = malloc(n);
    if (!p) inf_die("out of memory (%lu bytes)", (unsigned long) n);
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p;
    if (n == 0) n = 1;
    if (sz == 0) sz = 1;
    p = calloc(n, sz);
    if (!p) inf_die("out of memory (%lu x %lu bytes)",
                    (unsigned long) n, (unsigned long) sz);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q;
    if (n == 0) n = 1;
    q = p ? realloc(p, n) : malloc(n);
    if (!q) inf_die("out of memory (%lu bytes)", (unsigned long) n);
    return q;
}

char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *) xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* File loading                                                        */
/* ------------------------------------------------------------------ */

char *load_text_file(const char *path, char *errbuf, size_t errlen) {
    FILE *f;
    long n;
    char *buf;
    size_t got;

    if (errbuf && errlen) errbuf[0] = '\0';

    f = fopen(path, "rb");
    if (!f) {
        if (errbuf) sprintf(errbuf, "cannot open '%.100s'", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0 || n > 8L * 1024L * 1024L) {
        if (errbuf) sprintf(errbuf, "'%.60s' is not a plausible template", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (char *) xmalloc((size_t) n + 1);
    got = fread(buf, 1, (size_t) n, f);
    fclose(f);
    buf[got] = '\0';

    /* Templates are often stored with CRLF; Jinja does not care but the
     * literal text ends up in the prompt, so normalise to LF. */
    {
        char *r = buf, *w = buf;
        while (*r) {
            if (r[0] == '\r' && r[1] == '\n') r++;
            *w++ = *r++;
        }
        *w = '\0';
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* strbuf                                                              */
/* ------------------------------------------------------------------ */

void sb_init(strbuf *b) {
    b->data = (char *) xmalloc(256);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = 256;
}

void sb_free(strbuf *b) {
    if (b->data) free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void sb_clear(strbuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void sb_reserve(strbuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    while (b->cap < need) b->cap *= 2;
    b->data = (char *) xrealloc(b->data, b->cap);
}

int sb_add(strbuf *b, const char *s, size_t n) {
    if (n == 0) return 0;
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int sb_puts(strbuf *b, const char *s) {
    return sb_add(b, s, strlen(s));
}

int sb_printf(strbuf *b, const char *fmt, ...) {
    /* C89 has no vsnprintf. We format into a generously sized scratch
     * buffer; all call sites in infer produce short fragments (headers,
     * numbers, JSON scaffolding), never user text of unbounded length --
     * user text always goes through sb_add / sb_json_escape. */
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    return sb_add(b, tmp, strlen(tmp));
}

int sb_json_escape(strbuf *b, const char *s, size_t n) {
    size_t i;
    char esc[8];

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        switch (c) {
            case '"':  sb_add(b, "\\\"", 2); break;
            case '\\': sb_add(b, "\\\\", 2); break;
            case '\n': sb_add(b, "\\n", 2);  break;
            case '\r': sb_add(b, "\\r", 2);  break;
            case '\t': sb_add(b, "\\t", 2);  break;
            case '\b': sb_add(b, "\\b", 2);  break;
            case '\f': sb_add(b, "\\f", 2);  break;
            default:
                if (c < 0x20) {
                    sprintf(esc, "\\u%04x", (unsigned) c);
                    sb_add(b, esc, 6);
                } else {
                    /* UTF-8 bytes >= 0x20 pass through unchanged; JSON
                     * permits raw UTF-8 in strings. */
                    sb_add(b, (const char *) &c, 1);
                }
                break;
        }
    }
    return 0;
}
