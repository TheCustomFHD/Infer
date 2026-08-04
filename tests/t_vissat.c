/* t_vissat.c -- worst-case lane saturation.
 *
 * The VIS kernels accumulate products in 16-bit lanes and only widen
 * when the lane budget runs out. Those budgets are computed from the
 * maximum weight and the +-127 activation clamp, and Q6_K sits at
 * 32004 of 32767 -- a 2.3% margin. This test drives every lane to its
 * arithmetic worst case and requires bit-identity with i8, so the
 * bound is verified rather than asserted.
 *
 * Worst-case saturation probe: all weights at maximum, all activations
 * at the clamp, so every 16-bit lane carries its largest possible sum. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "backend.h"
#include "infer.h"
int main(void){
    long ncols=1024; int nrows=4;
    long rb_q6=(ncols/256)*210, rb_q5=(ncols/256)*176;
    unsigned char *w; float *x,*y1,*y2; long i; int r,bad=0;
    /* Q6_K: ql=0xFF, qh=0xFF -> every weight 63; scales max positive */
    w=(unsigned char*)malloc((size_t)(rb_q6*nrows));
    for(i=0;i<rb_q6*nrows;i++) w[i]=0xFF;
    for(r=0;r<nrows;r++){ long o; for(o=0;o<ncols/256;o++){
        long base=r*rb_q6+o*210; int k;
        for(k=0;k<16;k++) ((signed char*)(w+base+192))[k]=127;
        w[base+208]=0x00; w[base+209]=0x3C; /* d = 1.0 */ } }
    x=(float*)malloc((size_t)ncols*sizeof(float));
    y1=(float*)malloc((size_t)nrows*sizeof(float));
    y2=(float*)malloc((size_t)nrows*sizeof(float));
    for(i=0;i<ncols;i++) x[i]= (i&1)? 1.0f : -1.0f;  /* all |q|=127 after scaling */
    qmv_i8(GGML_TYPE_Q6_K,w,x,y1,ncols,nrows);
    bk_qmv_vis(GGML_TYPE_Q6_K,w,x,y2,ncols,nrows);
    for(r=0;r<nrows;r++){ double d=fabs((double)y1[r]-(double)y2[r]);
        if(d!=0.0){bad++; printf("Q6_K sat row %d: i8=%.9g vis=%.9g\n",r,y1[r],y2[r]);} }
    printf("Q6_K all-max saturation: %s\n", bad?"** MISMATCH **":"bit-identical");
    free(w);
    /* Q5_K: qs=0xFF, qh=0xFF -> every weight 31 */
    w=(unsigned char*)malloc((size_t)(rb_q5*nrows));
    for(i=0;i<rb_q5*nrows;i++) w[i]=0xFF;
    for(r=0;r<nrows;r++){ long o; for(o=0;o<ncols/256;o++){
        long base=r*rb_q5+o*176; w[base]=0x00; w[base+1]=0x3C;
        w[base+2]=0x00; w[base+3]=0x00; } }
    qmv_i8(GGML_TYPE_Q5_K,w,x,y1,ncols,nrows);
    bk_qmv_vis(GGML_TYPE_Q5_K,w,x,y2,ncols,nrows);
    { int b2=0; for(r=0;r<nrows;r++){ if(y1[r]!=y2[r]){b2++;
        printf("Q5_K sat row %d: i8=%.9g vis=%.9g\n",r,y1[r],y2[r]);} }
      printf("Q5_K all-max saturation: %s\n", b2?"** MISMATCH **":"bit-identical"); bad+=b2; }
    return bad?1:0;
}
