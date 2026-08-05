/* t_q5split.c -- the Q5_K split-loop form is integer-exact.
 *
 * i8_dot_q5_K walks its two nibble streams interleaved when compiled
 * scalar and split when compiled with SSE2/AVX2, because which shape
 * is faster depends on whether the vectoriser can take it (finding
 * 35). Both compute the same sums; this proves it, on the integer
 * accumulators, with no float in sight.
 *
 * Runs standalone; needs no model. */
#include <stdio.h>
static unsigned long rs=2718UL;
static unsigned int rnd(void){rs=rs*1103515245UL+12345UL;return (unsigned int)((rs>>16)&0xFFFFU);}
int main(void){
    unsigned char qs[32],qh[32]; short x1[32],x2[32];
    int t,bad=0;
    for(t=0;t<200000;t++){
        int l,sh1=(int)(rnd()&3)*2,sh2,a1=0,a2=0,b1=0,b2=0;
        sh2=sh1+1;
        for(l=0;l<32;l++){qs[l]=(unsigned char)(rnd()&0xFF);qh[l]=(unsigned char)(rnd()&0xFF);
            x1[l]=(short)((int)(rnd()%255)-127); x2[l]=(short)((int)(rnd()%255)-127);}
        for(l=0;l<32;l++){unsigned char v=qs[l];int h=qh[l];
            a1+=((int)(v&0x0F)+(((h>>sh1)&1)<<4))*x1[l];
            a2+=((int)(v>>4)  +(((h>>sh2)&1)<<4))*x2[l];}
        for(l=0;l<32;l++) b1+=((int)(qs[l]&0x0F)+(((qh[l]>>sh1)&1)<<4))*x1[l];
        for(l=0;l<32;l++) b2+=((int)(qs[l]>>4)  +(((qh[l]>>sh2)&1)<<4))*x2[l];
        if(a1!=b1||a2!=b2){bad++;if(bad<3)printf("  %d: %d/%d vs %d/%d\n",t,a1,a2,b1,b2);}
    }
    printf("200000 Q5_K sub-rows: %s\n",bad?"** MISMATCH **":"integer accumulators identical");
    return bad?1:0;}
