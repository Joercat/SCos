/* Freestanding boundary for upstream Lua. No host libc is linked. */
#ifndef SCOS_LUA_PORT_H
#define SCOS_LUA_PORT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <errno.h>
#include <setjmp.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "stb_sprintf.h"
int scos_lua_setjmp(void *) __attribute__((returns_twice));
void scos_lua_longjmp(void *, int) __attribute__((noreturn));
void scos_lua_abort(void) __attribute__((noreturn));
#undef setjmp
#define setjmp scos_lua_setjmp
#define longjmp scos_lua_longjmp
#define abort scos_lua_abort
#define snprintf stbsp_snprintf
#define sprintf stbsp_sprintf
#define strcoll strcmp
#define __sin scos_musl_sin
#define __cos scos_musl_cos
#define __tan scos_musl_tan
#define __sinl scos_musl_sinl
#define __cosl scos_musl_cosl
#define __tanl scos_musl_tanl
#define lua_getlocaledecpoint() ('.')
#define lua_writestring(s,n) ((void)(s),(void)(n))
#define lua_writeline() ((void)0)
#define lua_writestringerror(s,p) ((void)(s),(void)(p))
#define LUAI_MAXCCALLS 48
unsigned int scos_lua_seed(void);
struct lua_State;
void scos_lua_checkbudget(struct lua_State *L);
void scos_lua_charge(struct lua_State *L, size_t work);
#define luai_makeseed(L) ((void)(L),scos_lua_seed())
#define l_randomizePivot() scos_lua_seed()
/* Character classification is ASCII, not process/TLS locale state. */
#undef isalpha
#undef isalnum
#undef isdigit
#undef isspace
#undef iscntrl
#undef isgraph
#undef islower
#undef isupper
#undef ispunct
#undef isxdigit
#undef isprint
#undef tolower
#undef toupper
static inline int scos_isdigit(int c) { return ((unsigned)((c)-'0')<10); }
#define isdigit(c) scos_isdigit(c)
static inline int scos_islower(int c) { return ((unsigned)((c)-'a')<26); }
#define islower(c) scos_islower(c)
static inline int scos_isupper(int c) { return ((unsigned)((c)-'A')<26); }
#define isupper(c) scos_isupper(c)
static inline int scos_isalpha(int c) { return (islower(c)||isupper(c)); }
#define isalpha(c) scos_isalpha(c)
static inline int scos_isalnum(int c) { return (isalpha(c)||isdigit(c)); }
#define isalnum(c) scos_isalnum(c)
static inline int scos_isspace(int c) { return ((c)==' ' || (unsigned)((c)-'\t')<5); }
#define isspace(c) scos_isspace(c)
static inline int scos_iscntrl(int c) { return ((unsigned)(c)<32 || (c)==127); }
#define iscntrl(c) scos_iscntrl(c)
static inline int scos_isprint(int c) { return ((unsigned)((c)-32)<95); }
#define isprint(c) scos_isprint(c)
static inline int scos_isgraph(int c) { return ((unsigned)((c)-33)<94); }
#define isgraph(c) scos_isgraph(c)
static inline int scos_ispunct(int c) { return (isgraph(c)&&!isalnum(c)); }
#define ispunct(c) scos_ispunct(c)
static inline int scos_isxdigit(int c) { return (isdigit(c)||(unsigned)(((c)|32)-'a')<6); }
#define isxdigit(c) scos_isxdigit(c)
static inline int scos_tolower(int c) { return (isupper(c)?(c)+32:(c)); }
#define tolower(c) scos_tolower(c)
static inline int scos_toupper(int c) { return (islower(c)?(c)-32:(c)); }
#define toupper(c) scos_toupper(c)
#endif
