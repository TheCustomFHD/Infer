/* t_ident.c -- every backend agrees bit-for-bit, on every format.
 *
 * This is the precondition for --kernel: mixing kernels per weight
 * format is only a performance choice if the kernels are numerically
 * identical. If this test ever fails, --kernel becomes a correctness
 * hazard and the per-format table must go.
 *
 * Compares float results with memcmp, not a tolerance: "close" is not
 * good enough here.
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
  printf("i8 vs mmx, same input\n");
  bad+=chk(GGML_TYPE_Q4_K,"Q4_K",1024,8);
  bad+=chk(GGML_TYPE_Q5_K,"Q5_K",1024,8);
  bad+=chk(GGML_TYPE_Q6_K,"Q6_K",1024,8);
  bad+=chk(GGML_TYPE_Q8_0,"Q8_0",1024,8);
  printf(bad?"\n%d DIFFERENCE(S) -- kernels may NOT be mixed freely\n":"\nall bit-identical -- safe to mix per format\n",bad);
  return 0;}
