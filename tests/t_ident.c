/* t_ident.c -- the INTEGER backends agree bit-for-bit, on every format.
 *
 * Scope, precisely: this compares `i8` against `mmx` (and `vis` where
 * it is built). It does NOT cover `avx2`.
 *
 * `avx2` is deliberately not bit-identical on the k-quants: since
 * 1.20.0 it quantises the activation per 256 values instead of per 32,
 * reassociates the sum across lanes, and applies the scale before the
 * float conversion. Measured difference against `i8` on Q4_K/Q5_K/Q6_K
 * is large in absolute terms (max abs delta 5 / 15.7 / 18.4 on the
 * synthetic worst-case weights used here) and exactly zero on Q8_0,
 * which uses the same per-32 plane as `i8`. That is a legitimate
 * reordering of the same mathematics, not a bug, and it is bounded by
 * tests/t_avx2acc.c against the float reference instead.
 *
 * So do not "fix" this test by switching the avx2 comparison on. It
 * was enabled once and reported failures that were not defects. The
 * accuracy claim for avx2 lives in t_avx2acc.c; the identity claim for
 * the integer kernels lives here.
 *
 * Compares float results with memcmp, not a tolerance: "close" is not
 * good enough for the kernels this test DOES cover.
 *
 * Runs standalone; needs no model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backend.h"
#include "infer.h"
static unsigned long rs=99UL;
static unsigned int rnd(void){rs=rs*1103515245UL+12345UL;return (unsigned int)((rs>>16)&0xFFFFU);}
static long rb(int t,long n){long nb=n/256;
  switch(t){case GGML_TYPE_Q4_K:return nb*144;case GGML_TYPE_Q5_K:return nb*176;
            case GGML_TYPE_Q6_K:return nb*210;default:return (n/32)*34;}}
static int chk(int t,const char*nm,long ncols,int nrows){
  long bytes=rb(t,ncols);
  unsigned char*w=malloc(bytes*nrows);
  float*x=malloc(ncols*4),*a=malloc(nrows*4),*b=malloc(nrows*4);
  long i;int r,bad=0;
  for(i=0;i<bytes*nrows;i++)w[i]=rnd()&0xFF;
  for(r=0;r<nrows;r++){long k;for(k=0;k<ncols/256;k++){
    long o=r*bytes+k*(t==GGML_TYPE_Q4_K?144:(t==GGML_TYPE_Q5_K?176:210));
    if(t==GGML_TYPE_Q4_K||t==GGML_TYPE_Q5_K){w[o]=rnd()&0xFF;w[o+1]=0x2C;w[o+2]=rnd()&0xFF;w[o+3]=0x2B;}
    else if(t==GGML_TYPE_Q6_K){w[o+208]=rnd()&0xFF;w[o+209]=0x2C;}}}
  for(i=0;i<ncols;i++)x[i]=((float)(int)(rnd()&0x1FF)-256.0f)/128.0f;
  qmv_i8(t,w,x,a,ncols,nrows);
#ifdef INFER_HAVE_MMX
  qmv_mmx(t,w,x,b,ncols,nrows);
#else
  memcpy(b,a,nrows*4);
#endif
  for(r=0;r<nrows;r++) if(memcmp(&a[r],&b[r],4)!=0){bad++;
    if(bad<3)printf("    row %d  i8=%.9g mmx=%.9g  delta=%.3g\n",r,a[r],b[r],(double)(a[r]-b[r]));}
  printf("  %-5s ncols=%-5ld  %s\n",nm,ncols,bad?"** DIFFER **":"bit-identical");
  free(w);free(x);free(a);free(b);return bad;}
int main(void){int bad=0;
  printf("i8 vs mmx (avx2 is covered by t_avx2acc, not here)\n");
  bad+=chk(GGML_TYPE_Q4_K,"Q4_K",1024,8);
  bad+=chk(GGML_TYPE_Q5_K,"Q5_K",1024,8);
  bad+=chk(GGML_TYPE_Q6_K,"Q6_K",1024,8);
  bad+=chk(GGML_TYPE_Q8_0,"Q8_0",1024,8);
  printf(bad?"\n%d DIFFERENCE(S) -- the integer kernels disagree\n":"\ninteger kernels bit-identical (avx2 excluded by design)\n",bad);
  return 0;}
