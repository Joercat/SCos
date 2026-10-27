/* Tiny freestanding libc for the CPL3 app server.  No kernel calls. */
typedef unsigned long size_t;
typedef unsigned char u8;

void *memset(void *d, int c, size_t n)
{
    u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    u8 *p = d; const u8 *q = s;
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    u8 *p = d; const u8 *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *p = a, *q = b;
    while (n--) { if (*p != *q) return (int)*p - (int)*q; p++; q++; }
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    return n ? (int)(u8)*a - (int)(u8)*b : 0;
}
char *strcpy(char *d, const char *s) { char *o = d; while ((*d++ = *s++)); return o; }
char *strncpy(char *d, const char *s, size_t n)
{
    char *o = d;
    while (n && (*d++ = *s++)) n--;
    while (n--) *d++ = 0;
    return o;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
long sc(long nr, long a1, long a2, long a3, long a4, long a5);
_Noreturn void panic(const char *s)
{
    (void)s;
    sc(1, 1, 0, 0, 0, 0);       /* SC_NR_EXIT */
    for (;;) {}
}

/* The CPL3 CPU budget is enforced by the kernel's syscall dispatcher, so the
 * Lua-side cooperative hooks are no-ops here (they must still be defined). */
struct lua_State;
void scos_lua_checkbudget(struct lua_State *L) { (void)L; }
void scos_lua_charge(struct lua_State *L, unsigned long work) { (void)L; (void)work; }

/* lauxlib's file-loading paths are never reached (CAT source comes from the
 * shared page), but the object references them; provide inert definitions. */
typedef struct FILE FILE;
static char no_file[64];
FILE *fopen(const char *a, const char *b) { (void)a; (void)b; return (FILE *)0; }
FILE *freopen(const char *a, const char *b, FILE *c) { (void)a; (void)b; (void)c; return (FILE *)0; }
int fclose(FILE *f) { (void)f; return 0; }
int feof(FILE *f) { (void)f; return 1; }
int ferror(FILE *f) { (void)f; return 1; }
unsigned long fread(void *a, unsigned long b, unsigned long c, FILE *f)
{ (void)a; (void)b; (void)c; (void)f; return 0; }
int getc(FILE *f) { (void)f; return -1; }
void *realloc(void *p, unsigned long n) { (void)p; (void)n; return (void *)0; }
void free(void *p) { (void)p; }
char *strerror(int e) { (void)e; return (char *)"no libc"; }
FILE *stdin = (FILE *)no_file;
char *strstr(const char *a, const char *b)
{
    for (; *a; a++) { const char *p = a, *q = b;
        while (*p && *p == *q) { p++; q++; }
        if (!*q) return (char *)a; }
    return (char *)0;
}
