#include "kernel.h"
#include "../include/scos.h"
extern const uint8_t font8x16[256][16];
static struct boot_framebuffer fb;
static size_t row,col,columns,rows;
static uint32_t foreground,background;
void *memset(void *d,int c,size_t n){unsigned char *p=d;while(n--)*p++=(unsigned char)c;return d;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *p=d;const unsigned char *q=s;while(n--)*p++=*q++;return d;}
static uint32_t component(unsigned value,uint32_t mask){unsigned shift=0;while(!(mask&1)){mask>>=1;shift++;}return ((value*mask/255)<<shift);}
static uint32_t color(unsigned r,unsigned g,unsigned b){return component(r,fb.red)|component(g,fb.green)|component(b,fb.blue);}
void console_init(const struct boot_framebuffer *framebuffer){
 fb=*framebuffer;columns=fb.width/8;rows=fb.height/16;row=col=0;
#ifndef TESTABLE
 /* Tell the compositor where the screen is now, not when the desktop starts.  The boot log is the
  * first thing this machine scrolls, and it scrolls through the same scanout surface the desktop will
  * use; if the compositor is only told about it later, every line of the boot log is copied pixel by
  * pixel by the CPU even on a machine whose card can move it in one register write. */
 desktop_framebuffer(framebuffer);
#endif
 foreground=color(216,226,240);background=color(16,23,34);
 volatile uint32_t *p=(void*)(uintptr_t)fb.base;
 for(size_t y=0;y<fb.height;y++)for(size_t x=0;x<fb.width;x++)p[y*fb.stride+x]=background;
}
static void scroll(void){
 const size_t width=columns*8,band=(rows-1)*16;
#ifndef TESTABLE
 /* Ask the GPU to move the screen up a line first.  It is the same rectangle the fallback walks
  * pixel by pixel, and on a card whose own video RAM *is* the console surface the copy never
  * reaches the CPU at all: one line of a 1024x768 text screen is ~780 KB of uncached read-
  * modify-write the boot CPU no longer does.  No engine, an engine bound to a different card, or a
  * rectangle the engine declines all fall through to fb_scroll_cpu, so every machine scrolls. */
 if(fb.base&&rows>1){
  if(!fb_scroll_output(0,0,width,band,16,background))fb_scroll_cpu(0,0,width,band,16,background);
  row=rows-1;return;
 }
#endif
 volatile uint32_t *p=(void*)(uintptr_t)fb.base;
 for(size_t y=0;y<band;y++)for(size_t x=0;x<width;x++)p[y*fb.stride+x]=p[(y+16)*fb.stride+x];
 for(size_t y=band;y<rows*16;y++)for(size_t x=0;x<width;x++)p[y*fb.stride+x]=background;
 row=rows-1;
}
static void emit(char c){
 for(unsigned i=0;i<65536;i++)if(in8(0x3fd)&32){out8(0x3f8,(uint8_t)c);break;}
 if(!fb.base)return; /* Invalid handoff failures still have serial output. */
 /* Fatal NMI may interrupt cursor update/scroll; never trust transient bounds. */
 if(row>=rows)row=rows-1;
 if(col>=columns)col=0;
 if(c=='\n'){col=0;row++;}
 else if(c=='\r')col=0;
 else{
  volatile uint32_t *p=(void*)(uintptr_t)fb.base;
  for(size_t y=0;y<16;y++)for(size_t x=0;x<8;x++)p[(row*16+y)*fb.stride+col*8+x]=(font8x16[(uint8_t)c][y]&(128>>x))?foreground:background;
  if(++col==columns){col=0;row++;}
 }
 if(row>=rows)scroll();
}
void putstr(const char *s){while(*s)emit(*s++);}
void puthex(uint64_t n){putstr("0x");for(int i=60;i>=0;i-=4)emit("0123456789abcdef"[(n>>i)&15]);}
/* Fatal output owns scanout directly, even with a dead/corrupt compositor.
 * Clear only once so panic does not erase the preceding register dump. */
void console_fault_begin(void){
 static int started;
 if(started)return;
 started=1;
 if(fb.base)console_init(&fb);
}
extern const char *const panic_art[];
extern const int panic_art_lines;
_Noreturn void panic(const char *s){
 __asm__ volatile("cli");
 console_fault_begin();
 for(int i=0;i<panic_art_lines;i++){putstr("\n");putstr(panic_art[i]);}
 putstr("\nSCos PANIC: ");putstr(s);putstr("\nCPU halted. Power off manually.\n");
 for(;;)__asm__ volatile("hlt");
}
