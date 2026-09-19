#include "mtrr_plan.h"
extern int printf(const char *, ...);
int main(void)
{
    u64 b,m;u32 n;
    for(unsigned bits=32;bits<=52;bits++) {
        if(!mtrr_wc_plan(0xE0000000,3*1024*1024,bits,&b,&m,&n)) return 1;
        if((b&255)!=1 || n!=2*1024*1024 || !(m&(1ull<<11)) || (m>>bits)) return 2;
        if(!mtrr_wc_plan(0xE0200000,1024*1024,bits,&b,&m,&n) || n!=1024*1024) return 3;
    }
    if(mtrr_wc_plan(0xE0000001,4096,39,&b,&m,&n))return 4;
    if(mtrr_wc_plan(0xE0000000,4095,39,&b,&m,&n))return 5;
    if(mtrr_wc_plan(0xE0000000,4096,64,&b,&m,&n))return 6;
    printf("MTRR planner: WC type, valid bit, 32..52-bit masks and exact framebuffer bounds PASS\n");
    return 0;
}
