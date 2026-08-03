#include "../src/infer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
    char err[256]; qwen35_model*m; qwen35_ctx*c; qwen35_params p;
    int toks[512],n,i,pos=0; float*lg=NULL;
    const char*prompt = argc>2?argv[2]:"The capital of France is";
    m=qwen35_load(argv[1],err,sizeof(err));
    if(!m){fprintf(stderr,"load: %s\n",err);return 1;}
    printf("model=%s vocab=%d ctx_train=%d\n",qwen35_name(m),qwen35_n_vocab(m),qwen35_n_ctx_train(m));
    p.n_ctx=512;p.n_threads=1;
    c=qwen35_ctx_create(m,&p);
    n=tok_encode(qwen35_tokenizer(m),prompt,1,toks,512);
    printf("prompt=%s\ntokens(%d):",prompt,n);
    for(i=0;i<n;i++)printf(" %d",toks[i]);
    printf("\ndetok:");
    for(i=0;i<n;i++){int l;const char*s=tok_piece(qwen35_tokenizer(m),toks[i],&l);printf("[%.*s]",l,s);}
    printf("\n");
    for(i=0;i<n;i++) lg=qwen35_decode(c,toks[i],pos++,i==n-1);
    /* greedy continue */
    for(i=0;i<40;i++){
        int best=0,j;float bv=lg[0];
        for(j=1;j<qwen35_n_vocab(m);j++) if(lg[j]>bv){bv=lg[j];best=j;}
        {int l;const char*s=tok_piece(qwen35_tokenizer(m),best,&l);printf("%.*s",l,s);fflush(stdout);}
        if(best==tok_eos(qwen35_tokenizer(m)))break;
        lg=qwen35_decode(c,best,pos++,1);
    }
    printf("\n");
    return 0;
}
