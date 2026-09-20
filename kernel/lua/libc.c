#include "port.h"
#include "kernel.h"
#include "scan.h"
#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

void scos_lua_abort(void) { panic("unprotected Lua runtime failure"); }
/* The runtime is foreground-only and has no threads/TLS or locale changes. */
int *__errno_location(void) { static int value; return &value; }
double strtod(const char *s, char **end) {
    struct scos_scan f={(const unsigned char *)s,(const unsigned char *)s,0};
    double n=(double)__floatscan(&f,1,1);
    if(end)*end=(char *)f.p;
    return n;
}
void *memchr(const void *p,int c,size_t n) {
    const unsigned char *s=p;
    while(n--){if(*s==(unsigned char)c)return (void *)s;s++;}return NULL;
}
size_t strspn(const char *s,const char *accept) {
    size_t n=0;while(s[n]&&strchr(accept,s[n]))n++;return n;
}
size_t strcspn(const char *s,const char *reject) {
    size_t n=0;while(s[n]&&!strchr(reject,s[n]))n++;return n;
}
char *strpbrk(const char *s,const char *accept) {
    while(*s){if(strchr(accept,*s))return (char *)s;s++;}return NULL;
}

unsigned int scos_lua_seed(void) { uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));return lo^hi; }
int abs(int x){return x<0?-x:x;}
char *strchr(const char *s,int c){for(;;s++){if(*s==(char)c)return (char *)s;if(!*s)return NULL;}}
