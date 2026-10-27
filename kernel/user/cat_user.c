/* CPL3 CAT/Lua host.  This code runs at ring 3.  It provides the full API v2
 * `scos` table to a Lua 5.4 state; EVERY operation is a syscall with scalar
 * arguments.  There is no pointer to kernel memory anywhere in this file.
 *
 * The Lua VM itself is linked into this image (build/lua-runtime.o).  Its heap
 * is a fixed 1 MiB arena in this image's .bss, so a package can never allocate
 * outside its own process.
 */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "user_syscalls.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;

struct user_shared {
    u32 magic, event, app_id_len, source_len;
    u32 w, h, focused;
    u32 mouse_x, mouse_y, mouse_buttons;
    u32 keycode, key_pressed, key_shift, key_ctrl, key_alt;
    u32 tick_ms, surface_w, surface_h;
    u32 result, fault_count, denied_count, syscall_count;
    char app_id[32];
    char source[65536];
    char scratch[65536];
};
extern struct user_shared *u_sh;
extern long sc(long, long, long, long, long, long);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
extern unsigned long strlen(const char *);

static long C1(long n, long a)                  { return sc(n, a, 0, 0, 0, 0); }
static long C2(long n, long a, long b)          { return sc(n, a, b, 0, 0, 0); }
static long C3(long n, long a, long b, long c)  { return sc(n, a, b, c, 0, 0); }
static long C5(long n, long a, long b, long c, long d, long e) { return sc(n, a, b, c, d, e); }

/* Strings cross the boundary through the shared scratch, never by pointer. */
static long put(long off, const char *s, long n)
{
    if (off < 0 || n < 0 || off + n + 1 > 65536) return -1;
    memcpy(u_sh->scratch + off, s, (unsigned long)n);
    u_sh->scratch[off + n] = 0;
    return n;
}
static const char *get(long off) { return off >= 0 && off < 65536 ? u_sh->scratch + off : ""; }
static int streq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int integer(lua_State *L, int n, long lo, long hi)
{
    long v = (long)luaL_checkinteger(L, n);
    if (v < lo || v > hi) return luaL_error(L, "argument %d outside allowed range", n);
    return (int)v;
}
static u32 color(lua_State *L, int n) { return (u32)integer(L, n, 0, 0xffffff); }
static long pack2(long a, long b) { return (a << 32) | (b & 0xffffffffL); }

/* ---------------------------------------------------------------- drawing --- */
#define DRAW(n, cmd, a, b, c, d) C5(SC_NR_DRAW, (cmd), (a), (b), (c), (d))
static int a_clear(lua_State *L) { DRAW(0, SC_DRAW_CLEAR, color(L, 1), 0, 0, 0); return 0; }
static int a_rect(lua_State *L)
{ DRAW(0, SC_DRAW_RECT, integer(L,1,-4096,4096), integer(L,2,-4096,4096),
       pack2(integer(L,3,0,4096), integer(L,4,0,4096)), color(L,5)); return 0; }
static int a_frame(lua_State *L)
{ DRAW(0, SC_DRAW_FRAME, integer(L,1,-4096,4096), integer(L,2,-4096,4096),
       pack2(integer(L,3,0,4096), integer(L,4,0,4096)), color(L,5)); return 0; }
static int a_pixel(lua_State *L)
{ DRAW(0, SC_DRAW_PIXEL, integer(L,1,-4096,4096), integer(L,2,-4096,4096), color(L,3), 0); return 0; }
static int a_line(lua_State *L)
{ DRAW(0, SC_DRAW_LINE, integer(L,1,-4096,4096), integer(L,2,-4096,4096),
       pack2(integer(L,3,-4096,4096), integer(L,4,-4096,4096)), color(L,5)); return 0; }
static int a_circle(lua_State *L)
{ DRAW(0, SC_DRAW_CIRCLE, integer(L,1,-4096,4096), integer(L,2,-4096,4096),
       integer(L,3,0,1024), color(L,4)); return 0; }
static int a_disc(lua_State *L)
{ DRAW(0, SC_DRAW_DISC, integer(L,1,-4096,4096), integer(L,2,-4096,4096),
       integer(L,3,0,1024), color(L,4)); return 0; }
static int a_gradient(lua_State *L)
{ DRAW(0, SC_DRAW_GRADIENT, integer(L,1,-4096,4096), integer(L,2,-4096,4096),
       pack2(integer(L,3,0,4096), integer(L,4,0,4096)),
       (long)color(L,5) | ((long)color(L,6) << 32)); return 0; }
static int a_icon(lua_State *L)
{ DRAW(0, SC_DRAW_ICON, integer(L,1,-4096,4096), integer(L,2,-4096,4096),
       integer(L,3,0,23), color(L,4)); return 0; }
static int a_text(lua_State *L)
{ unsigned long n; const char *s = luaL_checklstring(L, 5, &n);
  put(0, s, (long)n);
  DRAW(0, SC_DRAW_TEXT, integer(L,1,-4096,4096), integer(L,2,-4096,4096), color(L,3), 0); return 0; }

/* ------------------------------------------------------------- pure helpers --- */
static int a_rgb(lua_State *L)
{ lua_pushinteger(L, ((long)integer(L,1,0,255) << 16) | ((long)integer(L,2,0,255) << 8) | integer(L,3,0,255)); return 1; }
static int a_blend(lua_State *L)
{ u32 a = color(L,1), b = color(L,2); long p = integer(L,3,0,100);
  long r = ((a>>16)&255)*(100-p)/100 + ((b>>16)&255)*p/100;
  long g = ((a>>8)&255)*(100-p)/100 + ((b>>8)&255)*p/100;
  long q = (a&255)*(100-p)/100 + (b&255)*p/100;
  lua_pushinteger(L, (r<<16)|(g<<8)|q); return 1; }
static int a_clamp(lua_State *L)
{ lua_Number x = luaL_checknumber(L,1), lo = luaL_checknumber(L,2), hi = luaL_checknumber(L,3);
  if (lo > hi) return luaL_error(L,"clamp bounds reversed");
  lua_pushnumber(L, x < lo ? lo : x > hi ? hi : x); return 1; }
static int a_hit(lua_State *L)
{ long x=luaL_checkinteger(L,1),y=luaL_checkinteger(L,2),rx=luaL_checkinteger(L,3),ry=luaL_checkinteger(L,4),
       w=luaL_checkinteger(L,5),h=luaL_checkinteger(L,6);
  lua_pushboolean(L, x>=rx&&y>=ry&&x<rx+w&&y<ry+h); return 1; }
static int a_size(lua_State *L) { lua_pushinteger(L,u_sh->surface_w); lua_pushinteger(L,u_sh->surface_h); return 2; }
static int a_screen(lua_State *L)
{ long v=C1(SC_NR_SCREEN,0); lua_pushinteger(L,(u64)v>>32); lua_pushinteger(L,v&0xffffffffL); return 2; }
static int a_window(lua_State *L)
{ lua_newtable(L);
  lua_pushinteger(L,u_sh->w); lua_setfield(L,-2,"width");
  lua_pushinteger(L,u_sh->h); lua_setfield(L,-2,"height");
  lua_pushinteger(L,0); lua_setfield(L,-2,"x");
  lua_pushinteger(L,0); lua_setfield(L,-2,"y");
  lua_pushinteger(L,0); lua_setfield(L,-2,"state");
  lua_pushboolean(L,u_sh->focused); lua_setfield(L,-2,"focused");
  return 1; }
static int a_mouse(lua_State *L)
{ lua_pushinteger(L,u_sh->mouse_x); lua_pushinteger(L,u_sh->mouse_y);
  lua_pushinteger(L,u_sh->mouse_buttons); return 3; }
static int a_clock(lua_State *L) { lua_pushnumber(L,(lua_Number)C1(SC_NR_CLOCK,0)/100.0); return 1; }
static int a_text_width(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  lua_pushinteger(L,C1(SC_NR_TEXT_WIDTH,0)); return 1; }
static int a_app_id(lua_State *L) { lua_pushstring(L,u_sh->app_id); return 1; }
static int a_focused(lua_State *L) { lua_pushboolean(L,u_sh->focused); return 1; }
static int a_interval(lua_State *L) { (void)integer(L,1,100,60000); return 0; }
static int a_info(lua_State *L)
{ lua_newtable(L);
  lua_pushinteger(L,2); lua_setfield(L,-2,"version");
  lua_pushinteger(L,65536); lua_setfield(L,-2,"source_limit");
  lua_pushinteger(L,0); lua_setfield(L,-2,"theme_permission");
  return 1; }
static int a_icons(lua_State *L)
{ static const char *n[]={"folder","terminal","notepad","browser","calendar","settings","info","cards","chart","solitaire"};
  lua_newtable(L); for (int i=0;i<10;i++){lua_pushinteger(L,i);lua_setfield(L,-2,n[i]);} return 1; }
static int a_icon_size(lua_State *L) { lua_pushinteger(L,24); lua_pushinteger(L,24); return 2; }
static int a_memory(lua_State *L) { lua_pushinteger(L,C1(SC_NR_MEMORY,0)); lua_pushinteger(L,2097152); return 2; }
static int a_date(lua_State *L)
{ long v=C1(SC_NR_DATE,0); lua_newtable(L);
  lua_pushinteger(L,(u64)v>>32); lua_setfield(L,-2,"year");
  lua_pushinteger(L,((u64)v>>24)&255); lua_setfield(L,-2,"month");
  lua_pushinteger(L,((u64)v>>16)&255); lua_setfield(L,-2,"day");
  lua_pushinteger(L,((u64)v>>8)&255); lua_setfield(L,-2,"hour");
  lua_pushinteger(L,(u64)v&255); lua_setfield(L,-2,"minute");
  return 1; }
static int a_log(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  C1(SC_NR_LOG,0); return 0; }
static int a_redraw(lua_State *L) { (void)L; C1(SC_NR_REDRAW,0); return 0; }
static int a_title(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  C1(SC_NR_TITLE,0); return 0; }
static int a_resize(lua_State *L)
{ lua_pushboolean(L,C2(SC_NR_RESIZE,integer(L,1,320,2048),integer(L,2,200,2048))); return 1; }
static int a_move(lua_State *L)
{ C2(SC_NR_MOVE,integer(L,1,-4096,4096),integer(L,2,-4096,4096)); return 0; }
static int a_minimize(lua_State *L) { (void)L; C1(SC_NR_MINIMIZE,0); return 0; }
static int a_notify(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  lua_pushboolean(L,C1(SC_NR_NOTIFY,0)); return 1; }
static int a_message(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  lua_pushboolean(L,C1(SC_NR_MESSAGE,0)); return 1; }
static int a_set_icon(lua_State *L) { C1(SC_NR_SET_ICON,integer(L,1,0,23)); return 0; }
static int a_theme(lua_State *L)
{ long c=C2(SC_NR_THEME_GET,0,0); lua_newtable(L);
  lua_pushstring(L,get(0)); lua_setfield(L,-2,"id");
  lua_pushinteger(L,c); lua_setfield(L,-2,"main"); return 1; }
static int a_themes(lua_State *L)
{ long n=C1(SC_NR_THEME_LIST,0); lua_newtable(L);
  for (long i=0;i<n&&i<16;i++){lua_pushinteger(L,i);lua_rawseti(L,-2,i+1);} return 1; }
static int a_theme_apply(lua_State *L)
{ unsigned long n; const char *s=luaL_checkstring(L,1); n=strlen(s); put(0,s,(long)n);
  lua_pushboolean(L,C1(SC_NR_THEME_APPLY,0)); return 1; }
static int a_theme_custom(lua_State *L) { lua_pushboolean(L,0); return 1; }
static int a_theme_status(lua_State *L) { lua_pushstring(L,"idle"); return 1; }

/* ------------------------------------------------------------------- files --- */
static int a_exists(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  lua_pushboolean(L,C1(SC_NR_FILE_EXISTS,0)!=0); return 1; }
static int a_file_size(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  long v=C1(SC_NR_FILE_SIZE,0); if(v<0)lua_pushnil(L); else lua_pushinteger(L,v); return 1; }
static int a_read(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  long got=C2(SC_NR_FILE_READ,0,1024);
  if(got<=0)lua_pushnil(L); else lua_pushlstring(L,get(1024),(unsigned long)got); return 1; }
static int a_write(lua_State *L)
{ unsigned long n,m; const char *s=luaL_checklstring(L,1,&n); const char *d=luaL_checklstring(L,2,&m);
  put(0,s,(long)n); put(256,d,(long)m);
  lua_pushboolean(L,C3(SC_NR_FILE_WRITE,0,256,(long)m)>0); return 1; }
static int a_remove(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n); put(0,s,(long)n);
  lua_pushboolean(L,C1(SC_NR_FILE_REMOVE,0)!=0); return 1; }
static int a_rename(lua_State *L)
{ unsigned long n,m; const char *s=luaL_checklstring(L,1,&n),*d=luaL_checklstring(L,2,&m);
  put(0,s,(long)n); put(128,d,(long)m);
  lua_pushboolean(L,C2(SC_NR_FILE_RENAME,0,128)!=0); return 1; }
static int a_files(lua_State *L)
{ long n=C2(SC_NR_FILE_LIST,1024,0); lua_newtable(L);
  if(n>0){ const char *p=get(1024); long start=0,i=1;
    for(long k=0;k<n;k++) if(p[k]=='\n'){ lua_pushlstring(L,p+start,k-start); lua_rawseti(L,-2,i++); start=k+1; } }
  return 1; }

static const luaL_Reg api[] = {
    {"clear",a_clear},{"rect",a_rect},{"frame",a_frame},{"pixel",a_pixel},{"line",a_line},
    {"circle",a_circle},{"disc",a_disc},{"gradient",a_gradient},{"icon",a_icon},{"text",a_text},
    {"rgb",a_rgb},{"blend",a_blend},{"clamp",a_clamp},{"hit",a_hit},{"size",a_size},
    {"screen",a_screen},{"window",a_window},{"mouse",a_mouse},{"time",a_clock},{"uptime",a_clock},
    {"text_width",a_text_width},{"app_id",a_app_id},{"focused",a_focused},{"interval",a_interval},
    {"api_info",a_info},{"info",a_info},{"icons",a_icons},{"icon_size",a_icon_size},
    {"memory",a_memory},{"date",a_date},{"log",a_log},{"redraw",a_redraw},{"title",a_title},
    {"resize",a_resize},{"move",a_move},{"minimize",a_minimize},{"notify",a_notify},
    {"message",a_message},{"set_icon",a_set_icon},{"theme",a_theme},{"themes",a_themes},
    {"theme_apply",a_theme_apply},{"theme_custom",a_theme_custom},{"theme_status",a_theme_status},
    {"exists",a_exists},{"file_size",a_file_size},{"read",a_read},{"write",a_write},
    {"remove",a_remove},{"rename",a_rename},{"files",a_files},{0,0}
};

/* --------------------------------------------------------------- Lua arena --- */
#define ARENA_BYTES (1024u * 1024u)
struct block { long size; struct block *next; int free; int pad; };
static unsigned char arena[ARENA_BYTES];
static void *alloc(void *ud, void *ptr, unsigned long old, unsigned long size)
{
    struct block *b = ptr ? (struct block *)ptr - 1 : 0;
    (void)ud;
    if (!size) {
        if (b) b->free = 1;
        for (struct block *p = (struct block *)arena; p && p->next; p = p->next)
            if (p->free && p->next->free) {
                p->size += (long)sizeof(struct block) + p->next->size;
                p->next = p->next->next;
            } else if (!p->next) break;
        return 0;
    }
    if (size > ARENA_BYTES - sizeof(struct block)) return 0;
    size = (size + 15) & ~15UL;
    if (b && (long)size <= b->size) return ptr;
    for (struct block *p = (struct block *)arena; p; p = p->next)
        if (p->free && (long)size <= p->size) {
            if (p->size >= (long)(size + sizeof(struct block) + 16)) {
                struct block *tail = (struct block *)((char *)(p + 1) + size);
                tail->size = p->size - (long)size - (long)sizeof(struct block);
                tail->next = p->next; tail->free = 1; tail->pad = 0;
                p->next = tail; p->size = (long)size;
            }
            p->free = 0;
            if (ptr) { memcpy(p + 1, ptr, old < size ? old : size); if (b) b->free = 1; }
            return p + 1;
        }
    return 0;
}
static const char *event_name(int e)
{
    switch (e) {
        case USER_EVENT_OPEN: return "open";
        case USER_EVENT_PAINT: return "paint";
        case USER_EVENT_KEY: return "key";
        case USER_EVENT_MOUSE: return "mouse";
        case USER_EVENT_TICK: return "tick";
        case USER_EVENT_CLOSE: return "close";
    }
    return 0;
}
static int initialize(lua_State *L)
{
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
    lua_getglobal(L, "string"); lua_pushnil(L); lua_setfield(L, -2, "dump"); lua_pop(L, 1);
    luaL_newlib(L, api);
    lua_pushinteger(L, 2); lua_setfield(L, -2, "version");
    lua_setglobal(L, "scos");
    return 0;
}
static int load_app(lua_State *L)
{
    if (luaL_loadbufferx(L, u_sh->source, u_sh->source_len, u_sh->app_id, "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) return luaL_error(L, "app source must return a callback table");
    lua_setglobal(L, "__scos_app");
    return 0;
}
static int dispatch(lua_State *L)
{
    const char *name = (const char *)lua_touserdata(L, 1);
    lua_getglobal(L, "__scos_app");
    lua_getfield(L, -1, name);
    if (lua_isfunction(L, -1)) {
        if (!streq(name, "paint")) { lua_pushinteger(L, u_sh->surface_w); lua_pushinteger(L, u_sh->surface_h); lua_call(L, 2, 0); }
        else if (!streq(name, "tick")) { lua_pushnumber(L, (lua_Number)u_sh->tick_ms / 100.0); lua_call(L, 1, 0); }
        else if (!streq(name, "key")) {
            lua_pushinteger(L, u_sh->keycode); lua_pushboolean(L, u_sh->key_pressed);
            lua_pushboolean(L, u_sh->key_shift); lua_pushboolean(L, u_sh->key_ctrl);
            lua_pushboolean(L, u_sh->key_alt); lua_call(L, 5, 0);
        } else if (!streq(name, "mouse")) {
            lua_pushinteger(L, 0); lua_pushinteger(L, u_sh->mouse_x); lua_pushinteger(L, u_sh->mouse_y);
            lua_pushinteger(L, 0); lua_pushboolean(L, 0); lua_pushinteger(L, 0);
            lua_pushinteger(L, u_sh->mouse_buttons); lua_call(L, 7, 0);
        } else lua_call(L, 0, 0);
    }
    return 0;
}

int user_cat_main(void)
{
    lua_State *L;
    const char *name = event_name((int)u_sh->event);
    if (!name) { C1(SC_NR_EXIT, 0); return 0; }
    ((struct block *)arena)->size = ARENA_BYTES - sizeof(struct block);
    ((struct block *)arena)->next = 0;
    ((struct block *)arena)->free = 1;
    L = lua_newstate(alloc, 0);
    if (!L) { u_sh->result = 1; C1(SC_NR_EXIT, 0); return 0; }
    lua_pushcfunction(L, initialize);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) goto fail;
    lua_pushcfunction(L, load_app);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) goto fail;
    lua_pushcfunction(L, dispatch);
    lua_pushlightuserdata(L, (void *)name);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) goto fail;
    u_sh->result = 0;
    C1(SC_NR_EXIT, 0);
    return 0;
fail:
    {
        const char *msg = lua_tostring(L, -1);
        unsigned long n = msg ? strlen(msg) : 0;
        if (n > 200) n = 200;
        if (n) { put(0, msg, (long)n); C1(SC_NR_LOG, 0); }
    }
    u_sh->result = 2;
    C1(SC_NR_EXIT, 0);
    return 0;
}
