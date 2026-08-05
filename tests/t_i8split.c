/* t_i8split.c -- the i8 Q4_K split-loop transformation is exact.
 *
 * i8_dot_q4_K used to walk the two nibble streams in one interleaved
 * loop; it now runs one clean reduction per stream. That is a pure
 * reassociation of independent integer sums, so it must be exact.
 *
 * Checked on the integer accumulators directly, with no float in
 * sight: comparing the float results instead would drag in x87 80-bit
 * excess precision on -mfpmath=387 builds and report differences that
 * are the compiler's spill choices, not the kernel's arithmetic.
 * (Finding 31.)
 *
 * Runs standalone; needs no model. */
#include <stdio.h>
#include <stdlib.h>
static unsigned long rs=7UL;
static unsigned int rnd(void){rs=rs*1103515245UL+12345UL;return (unsigned int)((rs>>16)&0xFFFFU);}
int main(void){
    unsigned char qs[32]; short x1[32],x2[32];
    int trial,bad=0;
    for(trial=0;trial<200000;trial++){
        int l,a1=0,a2=0,b1=0,b2=0;
        for(l=0;l<32;l++){qs[l]=(unsigned char)(rnd()&0xFF);
            x1[l]=(short)((int)(rnd()%255)-127); x2[l]=(short)((int)(rnd()%255)-127);}
        /* original: interleaved */
        for(l=0;l<32;l++){int w=qs[l]; a1+=(w&0xF)*x1[l]; a2+=(w>>4)*x2[l];}
        /* new: split */
        for(l=0;l<32;l++) b1+=(qs[l]&0xF)*x1[l];
        for(l=0;l<32;l++) b2+=(qs[l]>>4)*x2[l];
        if(a1!=b1||a2!=b2){bad++; if(bad<3)printf("  trial %d: %d/%d vs %d/%d\n",trial,a1,a2,b1,b2);}
    }
    printf("200000 random sub-rows: %s\n", bad?"** MISMATCH **":"integer accumulators identical");
    return bad?1:0;}
