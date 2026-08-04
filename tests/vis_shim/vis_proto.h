/* Shim reproducing Sun's vis_proto.h signatures (float/double, no
 * vector types) with C implementations, so the Sun Studio code path
 * can be type-checked and run without Sun Studio. */
#ifndef VIS_PROTO_H
#define VIS_PROTO_H
#include <string.h>

double vis_fmul8x16(float, double);
double vis_fpadd16(double, double);
double vis_fpadd32(double, double);
double vis_fmuld8ulx16(float, float);
double vis_fzero(void);

static int shim_ms16b(unsigned char a, short b) {
    return (int)(((int)a * (int)b + 0x80) >> 8);
}
double vis_fmul8x16(float fa, double db) {
    unsigned char ab[4]; short bs[4], rs[4]; double r;
    memcpy(ab, &fa, 4); memcpy(bs, &db, 8);
    { int i; for (i = 0; i < 4; i++) rs[i] = (short) shim_ms16b(ab[i], bs[i]); }
    memcpy(&r, rs, 8); return r;
}
double vis_fpadd16(double x, double y) {
    short a[4], b[4], c[4]; double r; int i;
    memcpy(a,&x,8); memcpy(b,&y,8);
    for (i=0;i<4;i++) c[i]=(short)(a[i]+b[i]);
    memcpy(&r,c,8); return r;
}
double vis_fpadd32(double x, double y) {
    int a[2], b[2], c[2]; double r; int i;
    memcpy(a,&x,8); memcpy(b,&y,8);
    for (i=0;i<2;i++) c[i]=a[i]+b[i];
    memcpy(&r,c,8); return r;
}
double vis_fmuld8ulx16(float fa, float fb) {
    unsigned char ab[4]; short bs[2]; int c[2]; double r; int i;
    memcpy(ab,&fa,4); memcpy(bs,&fb,4);
    /* fmuld8ulx16: u8 lanes 2,3 (low half) * s16 lanes, no rounding */
    for (i=0;i<2;i++) c[i] = (int)ab[i+2] * (int)bs[i];
    memcpy(&r,c,8); return r;
}
double vis_fzero(void) { double r; memset(&r,0,8); return r; }
#endif
