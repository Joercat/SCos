/* SCos adapter: musl floatscan's string-only stream interface. */
#ifndef SCOS_SCAN_H
#define SCOS_SCAN_H
#include <stddef.h>
struct scos_scan { const unsigned char *start, *p; int eof; };
static inline int shgetc(struct scos_scan *f) {
    if (!*f->p) { f->eof=1; return -1; }
    f->eof=0; return *f->p++;
}
static inline void shunget(struct scos_scan *f) {
    if (f->eof) f->eof=0;
    else if (f->p > f->start) f->p--;
}
static inline void shlim(struct scos_scan *f, int lim) {
    (void)lim; f->p=f->start; f->eof=0;
}
long double __floatscan(struct scos_scan *, int, int);
#endif
