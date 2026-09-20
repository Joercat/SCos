#include "kernel.h"
static volatile uint16_t *const video=(volatile uint16_t *)0xb8000;
static size_t row,col;
void *memset(void *d,int c,size_t n) { unsigned char *p=d; while(n--) *p++=(unsigned char)c; return d; }
void *memcpy(void *d,const void *s,size_t n) { unsigned char *p=d; const unsigned char *q=s; while(n--) *p++=*q++; return d; }
void console_init(void) {
    row=col=0;
    for(size_t i=0;i<80*25;i++) video[i]=0x0720;
    out8(0x3d4,0x0a); out8(0x3d5,0x20); /* hide firmware cursor */
}
static void emit(char c) {
    /* Missing UART must not hang a boot or panic. */
    for(unsigned i=0;i<65536;i++) if(in8(0x3fd)&0x20) {out8(0x3f8,(uint8_t)c);break;}
    if(c=='\n') {col=0;row++;}
    else if(c!='\r') {video[row*80+col]=(uint16_t)(0x0700|(uint8_t)c);if(++col==80){col=0;row++;}}
    if(row==25) {
        for(size_t i=0;i<80*24;i++) video[i]=video[i+80];
        for(size_t i=80*24;i<80*25;i++) video[i]=0x0720;
        row=24;
    }
}
void putstr(const char *s) {while(*s) emit(*s++);}
void puthex(uint64_t n) {putstr("0x");for(int i=60;i>=0;i-=4) emit("0123456789abcdef"[(n>>i)&15]);}
_Noreturn void panic(const char *s) {
    __asm__ volatile("cli");
    putstr("\nSCos PANIC: ");putstr(s);putstr("\nCPU halted. Power off manually.\n");
    for(;;) __asm__ volatile("hlt");
}
