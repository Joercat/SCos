/* Host-side checks of the actual CPUID brand assembly and buffer bounds. */
#include "scos.h"
extern int printf(const char *, ...);
int main(void)
{
    u32 words[12], maxleaf, b, c, d;
    __asm__ volatile("cpuid" : "=a"(maxleaf), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (maxleaf < 0x80000004u) return 0;
    for (unsigned i = 0; i < 3; i++)
        __asm__ volatile("cpuid" : "=a"(words[4*i]), "=b"(words[4*i+1]),
                         "=c"(words[4*i+2]), "=d"(words[4*i+3])
                         : "a"(0x80000002u+i), "c"(0));
    char expected[49]; memcpy(expected, words, 48); expected[48] = 0;
    char *p = expected; while (*p == ' ') p++;
    int n = strlen(p); while (n && p[n-1] == ' ') p[--n] = 0;
    char full[49]; cpu_brand(full, sizeof(full));
    if (strcmp(full,p)) { printf("FAIL brand: %s != %s\n",full,p); return 1; }
    for (int size = 0; size <= 49; size++) {
        unsigned char buf[51]; memset(buf, 0xAB, sizeof(buf));
        cpu_brand((char *)buf+1, size);
        if (buf[0] != 0xAB || buf[size+1] != 0xAB) return 2;
        if (size && strlen((char *)buf+1) >= (unsigned)size) return 3;
    }
    cpu_brand(0,0);
    printf("CPU brand: %s; all buffer-size guards PASS\n", full);
    return 0;
}
