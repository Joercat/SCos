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
    u32 mouse_event, mouse_button, mouse_down, mouse_wheel;
    u32 window_x, window_y, window_state;
};
extern struct user_shared *u_sh;
extern long sc(long, long, long, long, long, long);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
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
/* API v2 must be the SAME API whether the package runs in the kernel-side host
 * (owner chose "never isolated") or here at CPL3.  The argument order, ranges
 * and error text below are therefore a mirror of kernel/desktop/lua_api.inc,
 * not an independent design.  Every drawing call is one SC_NR_DRAW syscall.
 */
/* A refused primitive is an ERROR, not a silent no-op: the kernel-side host
 * raises "drawing budget exceeded" too, so a package cannot tell which host it
 * is running in by whether an over-budget draw fails loudly.  The kernel
 * answers -1 for both a spent budget and a missing surface. */
static void draw_call(lua_State *L, long cmd, long a, long b, long c, long d)
{
    if (C5(SC_NR_DRAW, cmd, a, b, c, d) < 0)
        luaL_error(L, "drawing budget exceeded");
}
#define DRAW(L, cmd, a, b, c, d) draw_call((L), (cmd), (a), (b), (c), (d))
#define DISABLED "isolated app: this operation is disabled by the owner"

static int painting;          /* set for the paint event only */
static void need_paint(lua_State *L)
{
    if (!painting) luaL_error(L, "drawing is only allowed in paint");
}
static int xy(lua_State *L, int n) { return integer(L, n, -4096, 4096); }
static int extent(lua_State *L, int n) { return integer(L, n, 0, 4096); }
static const char *label(lua_State *L, int n)
{
    unsigned long len; const char *s = luaL_checklstring(L, n, &len);
    if (len > 1024) luaL_error(L, "text exceeds 1024 bytes");
    return s;
}
static long text_put(const char *s)
{
    return put(0, s, (long)strlen(s));
}

static int a_clear(lua_State *L) { need_paint(L); DRAW(L, SC_DRAW_CLEAR, color(L,1), 0, 0, 0); return 0; }
static int a_rect(lua_State *L)
{ need_paint(L);
  DRAW(L, SC_DRAW_RECT, xy(L,1), xy(L,2), pack2(extent(L,3), extent(L,4)), color(L,5)); return 0; }
static int a_frame(lua_State *L)
{ need_paint(L);
  DRAW(L, SC_DRAW_FRAME, xy(L,1), xy(L,2), pack2(extent(L,3), extent(L,4)), color(L,5)); return 0; }
static int a_pixel(lua_State *L)
{ need_paint(L); DRAW(L, SC_DRAW_PIXEL, xy(L,1), xy(L,2), color(L,3), 0); return 0; }
static int a_line(lua_State *L)
{ need_paint(L);
  DRAW(L, SC_DRAW_LINE, xy(L,1), xy(L,2), pack2(xy(L,3), xy(L,4)), color(L,5)); return 0; }
static int a_circle(lua_State *L)
{ need_paint(L);
  DRAW(L, SC_DRAW_CIRCLE, xy(L,1), xy(L,2), integer(L,3,0,1024), color(L,4)); return 0; }
static int a_disc(lua_State *L)
{ need_paint(L);
  DRAW(L, SC_DRAW_DISC, xy(L,1), xy(L,2), integer(L,3,0,1024), color(L,4)); return 0; }
static int a_gradient(lua_State *L)
{ need_paint(L);
  DRAW(L, SC_DRAW_GRADIENT, xy(L,1), xy(L,2), pack2(extent(L,3), extent(L,4)),
       (long)color(L,5) | ((long)color(L,6) << 32)); return 0; }
/* scos.icon(id, x, y, colour) - id first, exactly as in the kernel host. */
static int a_icon(lua_State *L)
{ need_paint(L);
  DRAW(L, SC_DRAW_ICON, integer(L,1,0,9), xy(L,2), xy(L,3), color(L,4)); return 0; }
static int a_text(lua_State *L)
{ need_paint(L);
  const char *s = label(L,3); text_put(s);
  DRAW(L, SC_DRAW_TEXT, xy(L,1), xy(L,2), color(L,4), 0); return 0; }
static int a_text_scaled(lua_State *L)
{ need_paint(L);
  const char *s = label(L,3); text_put(s);
  DRAW(L, SC_DRAW_TEXT_SCALED, xy(L,1), xy(L,2), color(L,4), integer(L,5,1,4)); return 0; }
static int a_text_clip(lua_State *L)
{ need_paint(L);
  const char *s = label(L,4); text_put(s);
  DRAW(L, SC_DRAW_TEXT_CLIP, xy(L,1), xy(L,2), extent(L,3), color(L,5)); return 0; }
static int a_text_wrap(lua_State *L)
{ need_paint(L);
  int x = xy(L,1), y = xy(L,2), width = extent(L,3), height = extent(L,4);
  const char *s = label(L,5); u32 c = color(L,6);
  int cols = width / 8, rows = height / 16, used = 0;
  if (cols > 127) cols = 127;
  while (*s && cols > 0 && used < rows) {
      char line[128]; int n = 0;
      while (*s && *s != '\n' && n < cols) { char ch = *s++; if (ch != '\r') line[n++] = ch; }
      if (*s == '\n') s++;
      line[n] = 0;
      put(0, line, (long)n);
      DRAW(L, SC_DRAW_TEXT, x, y + used * 16, (long)c, 0);
      used++;
  }
  lua_pushinteger(L, used); lua_pushboolean(L, !*s); return 2;
}

static long blend_colour(long a, long b, long p)
{
    long r = ((a >> 16) & 255) * (255 - p) / 255 + ((b >> 16) & 255) * p / 255;
    long g = ((a >> 8) & 255) * (255 - p) / 255 + ((b >> 8) & 255) * p / 255;
    long q = (a & 255) * (255 - p) / 255 + (b & 255) * p / 255;
    return (r << 16) | (g << 8) | q;
}

/* The widgets below are drawn from the live theme, like the kernel versions. */
static void theme_values_local(u32 v[10], char *id, unsigned long cap)
{
    unsigned long i = 0;
    C1(SC_NR_THEME_GET, 0);
    memcpy(v, u_sh->scratch + 40, 40);
    if (id && cap) {
        while (i + 1 < cap && u_sh->scratch[i]) { id[i] = u_sh->scratch[i]; i++; }
        id[i] = 0;
    }
}
static int a_button(lua_State *L)
{
    need_paint(L);
    int x = xy(L,1), y = xy(L,2), w = extent(L,3), h = extent(L,4);
    const char *s = label(L,5); int active = lua_toboolean(L,6);
    u32 v[10]; theme_values_local(v, 0, 0);
    if (w && h) {
        u32 fill = active ? v[0] : blend_colour(v[3], v[0], 18);
        DRAW(L, SC_DRAW_RECT, x, y, pack2(w, h), fill);
        DRAW(L, SC_DRAW_FRAME, x, y, pack2(w, h), v[0]);
        if (h >= 16) {
            int tw = (int)strlen(s) * 8;
            int tx = x + 8, clip = w > 16 ? w - 16 : 0;
            if (tw > clip) { char tmp[128]; int k = 0; while (k < clip / 8 && s[k] && k < 127) { tmp[k] = s[k]; k++; } tmp[k] = 0; s = tmp; }
            put(0, s, (long)strlen(s));
            DRAW(L, SC_DRAW_TEXT_CLIP, tx, y + (h - 16) / 2, (long)clip,
                 active ? v[5] : v[4]);
        }
    }
    return 0;
}
static int a_checkbox(lua_State *L)
{
    need_paint(L);
    int x = xy(L,1), y = xy(L,2); const char *s = label(L,3); int checked = lua_toboolean(L,4);
    u32 v[10]; theme_values_local(v, 0, 0);
    DRAW(L, SC_DRAW_FRAME, x, y, pack2(16, 16), v[0]);
    if (checked) DRAW(L, SC_DRAW_RECT, x + 4, y + 4, pack2(8, 8), v[0]);
    put(0, s, (long)strlen(s));
    DRAW(L, SC_DRAW_TEXT, x + 24, y, (long)v[4], 0);
    return 0;
}
static int a_progress(lua_State *L)
{
    need_paint(L);
    int x = xy(L,1), y = xy(L,2), w = extent(L,3), h = extent(L,4), p = integer(L,5,0,100);
    u32 v[10]; theme_values_local(v, 0, 0);
    if (w && h) {
        DRAW(L, SC_DRAW_RECT, x, y, pack2(w, h), v[3]);
        DRAW(L, SC_DRAW_RECT, x, y, pack2(w * p / 100, h), v[0]);
        DRAW(L, SC_DRAW_FRAME, x, y, pack2(w, h), v[4]);
    }
    return 0;
}
/* ------------------------------------------------------------- pure helpers --- */
static int a_rgb(lua_State *L)
{ lua_pushinteger(L, ((long)integer(L,1,0,255) << 16) | ((long)integer(L,2,0,255) << 8) | integer(L,3,0,255)); return 1; }
static int a_blend(lua_State *L)
{ u32 a = color(L,1), b = color(L,2); long p = integer(L,3,0,100);
  lua_pushinteger(L, blend_colour(a, b, p * 255 / 100)); return 1; }
static int a_clamp(lua_State *L)
{ lua_Number x = luaL_checknumber(L,1), lo = luaL_checknumber(L,2), hi = luaL_checknumber(L,3);
  if (lo > hi) return luaL_error(L,"clamp bounds reversed");
  lua_pushnumber(L, x < lo ? lo : x > hi ? hi : x); return 1; }
static int a_hit(lua_State *L)
{ long x=xy(L,1),y=xy(L,2),rx=xy(L,3),ry=xy(L,4),w=extent(L,5),h=extent(L,6);
  lua_pushboolean(L, x>=rx&&y>=ry&&x<rx+w&&y<ry+h); return 1; }
static int a_size(lua_State *L) { lua_pushinteger(L,u_sh->surface_w); lua_pushinteger(L,u_sh->surface_h); return 2; }
static int a_screen(lua_State *L)
{ long v=C1(SC_NR_SCREEN,0); lua_pushinteger(L,(u64)v>>32); lua_pushinteger(L,v&0xffffffffL); return 2; }
static int a_window(lua_State *L)
{ lua_newtable(L);
  lua_pushinteger(L,u_sh->window_x); lua_setfield(L,-2,"x");
  lua_pushinteger(L,u_sh->window_y); lua_setfield(L,-2,"y");
  lua_pushinteger(L,u_sh->w); lua_setfield(L,-2,"width");
  lua_pushinteger(L,u_sh->h); lua_setfield(L,-2,"height");
  lua_pushinteger(L,u_sh->window_state); lua_setfield(L,-2,"state");
  lua_pushboolean(L,u_sh->focused); lua_setfield(L,-2,"focused");
  return 1; }
static int a_mouse(lua_State *L)
{ lua_pushinteger(L,u_sh->mouse_x); lua_pushinteger(L,u_sh->mouse_y);
  lua_pushinteger(L,u_sh->focused ? u_sh->mouse_buttons : 0); return 3; }
static int a_clock(lua_State *L)
{ /* SC_NR_CLOCK is milliseconds; scos.time()/uptime() are seconds. */
  lua_pushnumber(L,(lua_Number)C1(SC_NR_CLOCK,0)/1000.0); return 1; }
static int a_text_width(lua_State *L)
{ const char *s=label(L,1); text_put(s); lua_pushinteger(L,C1(SC_NR_TEXT_WIDTH,0)); return 1; }
static int a_app_id(lua_State *L) { lua_pushstring(L,u_sh->app_id); return 1; }
static int a_focused(lua_State *L) { lua_pushboolean(L,u_sh->focused); return 1; }
static int a_interval(lua_State *L) { C1(SC_NR_INTERVAL, integer(L,1,100,60000)); return 0; }
static int a_info(lua_State *L)
{ lua_newtable(L);
  lua_pushinteger(L,2); lua_setfield(L,-2,"version");
  lua_pushinteger(L,65536); lua_setfield(L,-2,"source_limit");
  lua_pushinteger(L,2097152); lua_setfield(L,-2,"arena_limit");
  lua_pushinteger(L,65536); lua_setfield(L,-2,"data_limit");
  lua_pushinteger(L,16); lua_setfield(L,-2,"data_files");
  lua_pushinteger(L,0); lua_setfield(L,-2,"theme_permission");
  /* This host only ever runs at CPL3 with a capability set, which is what the
   * kernel-side host calls "isolated".  A package can therefore tell whether
   * its sandbox is active without being able to change it. */
  lua_pushboolean(L,1); lua_setfield(L,-2,"isolated");
  return 1; }
static int a_icons(lua_State *L)
{ static const char *n[]={"folder","terminal","notepad","browser","calendar","settings","info","cards","chart","solitaire"};
  lua_newtable(L); for (int i=0;i<10;i++){lua_pushinteger(L,i);lua_setfield(L,-2,n[i]);} return 1; }
static int a_icon_size(lua_State *L) { lua_pushinteger(L,24); lua_pushinteger(L,24); return 2; }
static int a_memory(lua_State *L) { lua_pushinteger(L,C1(SC_NR_MEMORY,0)); lua_pushinteger(L,2097152); return 2; }
static int a_date(lua_State *L)
{ u32 f[7]; C1(SC_NR_DATE,0);
  memcpy(f, u_sh->scratch, sizeof f);
  lua_newtable(L);
  lua_pushinteger(L,f[0]); lua_setfield(L,-2,"year");
  lua_pushinteger(L,f[1]); lua_setfield(L,-2,"month");
  lua_pushinteger(L,f[2]); lua_setfield(L,-2,"day");
  lua_pushinteger(L,f[3]); lua_setfield(L,-2,"hour");
  lua_pushinteger(L,f[4]); lua_setfield(L,-2,"minute");
  lua_pushinteger(L,f[5]); lua_setfield(L,-2,"second");
  lua_pushinteger(L,f[6]); lua_setfield(L,-2,"weekday");
  return 1; }
static int a_log(lua_State *L)
{ const char *s=label(L,1);
  if (strlen(s) > 512) return luaL_error(L,"log exceeds 512 bytes");
  text_put(s); C1(SC_NR_LOG,0); return 0; }
static int a_redraw(lua_State *L) { (void)L; C1(SC_NR_REDRAW,0); return 0; }
static int a_title(lua_State *L)
{ unsigned long n; const char *s=luaL_checklstring(L,1,&n);
  if (n > 63) return luaL_error(L,"title exceeds 63 bytes");
  put(0,s,(long)n); C1(SC_NR_TITLE,0); return 0; }
static int a_resize(lua_State *L)
{ if (painting) return luaL_error(L,"resize is not allowed in paint");
  lua_pushboolean(L,C2(SC_NR_RESIZE,integer(L,1,320,2048),integer(L,2,200,2048))); return 1; }
static int a_move(lua_State *L)
{ if (painting) return luaL_error(L,"move is not allowed in paint");
  lua_pushboolean(L, C2(SC_NR_MOVE,xy(L,1),xy(L,2))); return 1; }
static int a_minimize(lua_State *L)
{ (void)L; if (painting) return luaL_error(L,"minimize is not allowed in paint");
  C1(SC_NR_MINIMIZE,0); return 0; }
static int a_notify(lua_State *L)
{ const char *s=label(L,1);
  if (strlen(s) > 192) return luaL_error(L,"notification exceeds 192 bytes");
  if (painting) return luaL_error(L,"notifications cannot be requested in paint");
  put(0,s,(long)strlen(s));
  { long r = C1(SC_NR_NOTIFY,0); if (r < 0) return luaL_error(L,DISABLED); lua_pushboolean(L,r > 0); }
  return 1; }
static int a_message(lua_State *L)
{ const char *s=label(L,1);
  if (strlen(s) > 192) return luaL_error(L,"message exceeds 192 bytes");
  if (painting) return luaL_error(L,"messages cannot be requested in paint");
  put(0,s,(long)strlen(s));
  { long r = C1(SC_NR_MESSAGE,0); if (r < 0) return luaL_error(L,DISABLED); lua_pushboolean(L,r > 0); }
  return 1; }
static int a_set_icon(lua_State *L)
{ if (painting) return luaL_error(L,"set_icon is not allowed in paint");
  if (C1(SC_NR_SET_ICON,integer(L,1,0,9)) < 0) return luaL_error(L,DISABLED);
  return 0; }
static int a_theme(lua_State *L)
{ static const char *fields[]={"main","bg_top","bg_bot","win_bg","text","title_text","taskbar_bg","mode","spacing","grid"};
  u32 v[10]; char id[33];
  theme_values_local(v, id, sizeof id);
  lua_newtable(L);
  lua_pushstring(L,id); lua_setfield(L,-2,"id");
  for (int i=0;i<10;i++){lua_pushinteger(L,v[i]);lua_setfield(L,-2,fields[i]);}
  return 1; }
static int a_themes(lua_State *L)
{ /* The names arrive newline separated; scos.themes() is a list of IDs. */
  long n=C1(SC_NR_THEME_LIST,0); lua_newtable(L);
  const char *p=get(0); long start=0,i=1;
  for(long k=0;k<65535&&(char)p[k];k++)
    if(p[k]=='\n'){ lua_pushlstring(L,p+start,k-start); lua_rawseti(L,-2,i++); start=k+1; }
  (void)n; return 1; }
static long theme_known(const char *s)
{
    C1(SC_NR_THEME_LIST,0);
    const char *p=get(0); long i=0;
    while (*p) { const char *e=p; while(*e&&*e!='\n')e++;
        if ((unsigned long)(e-p)==strlen(s) && !memcmp(p,s,(unsigned long)(e-p))) return 1;
        p = *e ? e+1 : e; i++; }
    (void)i; return 0;
}
static int a_theme_apply(lua_State *L)
{ const char *s=luaL_checkstring(L,1);
  if (strlen(s) > 30 || !theme_known(s)) return luaL_error(L,"unknown theme");
  put(0,s,(long)strlen(s));
  { long r = C1(SC_NR_THEME_APPLY,0); if (r < 0) return luaL_error(L,DISABLED); lua_pushboolean(L,r > 0); }
  return 1; }
static int a_theme_custom(lua_State *L)
{ (void)L; return luaL_error(L,DISABLED); }
static int a_theme_status(lua_State *L)
{ C1(SC_NR_THEME_STATUS,0); lua_pushstring(L,get(0)); return 1; }

/* ------------------------------------------------------------------- files --- */
/* A data file name is a basename validated by the kernel's mount namespace.
 * Reject what it rejects here, so the error arrives as a Lua error instead of
 * a silently truncated name. */
static const char *file_name(lua_State *L, int n)
{
    unsigned long len; const char *s = luaL_checklstring(L, n, &len);
    if (!len || len >= 64 || strlen(s) != len) luaL_error(L, "invalid data filename");
    if (streq(s, ".") || streq(s, "..")) luaL_error(L, "invalid data filename");
    return s;
}
static int a_exists(lua_State *L)
{ const char *s=file_name(L,1); put(0,s,(long)strlen(s));
  { long r = C1(SC_NR_FILE_EXISTS,0);
    if (r < 0) return luaL_error(L,DISABLED);
    lua_pushboolean(L, r > 0); }
  return 1; }
static int a_file_size(lua_State *L)
{ const char *s=file_name(L,1); put(0,s,(long)strlen(s));
  { long r = C1(SC_NR_FILE_SIZE,0);
    if (r == -1) return luaL_error(L,DISABLED);
    if (r < 0) lua_pushnil(L); else lua_pushinteger(L,r); }
  return 1; }
static int a_read(lua_State *L)
{ const char *s=file_name(L,1); put(0,s,(long)strlen(s));
  { long size=C1(SC_NR_FILE_SIZE,0);
    if (size == -1) return luaL_error(L,DISABLED);
    if (size > 65536) return luaL_error(L,"data file too large");
    /* The name has done its job once the kernel resolved the path, so the
     * data can start at offset 0 and use the whole 64 KiB window. */
    long got=C2(SC_NR_FILE_READ,0,0);
    if (got == -1) return luaL_error(L,DISABLED);
    if (got < 0) lua_pushnil(L); else lua_pushlstring(L,get(0),(unsigned long)got); }
  return 1; }
static int a_write(lua_State *L)
{ unsigned long n,m; const char *s=file_name(L,1); const char *d=luaL_checklstring(L,2,&m);
  n=strlen(s);
  if (m > 65535 - 256) return luaL_error(L,"data file too large");
  put(0,s,(long)n); put(256,d,(long)m);
  { long r=C3(SC_NR_FILE_WRITE,0,256,(long)m);
    if (r < 0) return luaL_error(L,DISABLED);
    lua_pushboolean(L, r > 0 || m == 0); }
  return 1; }
static int a_remove(lua_State *L)
{ const char *s=file_name(L,1); put(0,s,(long)strlen(s));
  { long r=C1(SC_NR_FILE_REMOVE,0);
    if (r < 0) return luaL_error(L,DISABLED);
    lua_pushboolean(L,r > 0); }
  return 1; }
static int a_rename(lua_State *L)
{ const char *s=file_name(L,1),*d=file_name(L,2);
  put(0,s,(long)strlen(s)); put(128,d,(long)strlen(d));
  { long r=C2(SC_NR_FILE_RENAME,0,128);
    if (r < 0) return luaL_error(L,DISABLED);
    lua_pushboolean(L,r > 0); }
  return 1; }
static int a_files(lua_State *L)
{ long n=C2(SC_NR_FILE_LIST,1024,0);
  if (n < 0) return luaL_error(L,DISABLED);
  lua_newtable(L);
  if(n>0){ const char *p=get(1024); long start=0,i=1;
    for(long k=0;k<n;k++) if(p[k]=='\n'){ lua_pushlstring(L,p+start,k-start); lua_rawseti(L,-2,i++); start=k+1; } }
  return 1; }

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
    {"remove",a_remove},{"rename",a_rename},{"files",a_files},
    {"text_scaled",a_text_scaled},{"text_clip",a_text_clip},{"text_wrap",a_text_wrap},
    {"button",a_button},{"checkbox",a_checkbox},{"progress",a_progress},
    {0,0}
};

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
/* The same argument list the kernel-side host passes for each event, so a
 * package behaves identically whichever way it was launched. */
static int dispatch(lua_State *L)
{
    const char *name = (const char *)lua_touserdata(L, 1);
    int n = 0;
    painting = streq(name, "paint");
    lua_getglobal(L, "__scos_app");
    lua_getfield(L, -1, name);
    if (lua_isfunction(L, -1)) {
        if (streq(name, "paint")) {
            lua_pushinteger(L, u_sh->surface_w); lua_pushinteger(L, u_sh->surface_h); n = 2;
        } else if (streq(name, "tick")) {
            lua_pushnumber(L, (lua_Number)u_sh->tick_ms / 1000.0); n = 1;
        } else if (streq(name, "key")) {
            lua_pushinteger(L, u_sh->keycode);
            lua_pushboolean(L, u_sh->key_pressed);
            lua_pushboolean(L, u_sh->key_shift);
            lua_pushboolean(L, u_sh->key_ctrl);
            lua_pushboolean(L, u_sh->key_alt); n = 5;
        } else if (streq(name, "mouse")) {
            lua_pushinteger(L, u_sh->mouse_event);
            lua_pushinteger(L, u_sh->mouse_x);
            lua_pushinteger(L, u_sh->mouse_y);
            lua_pushinteger(L, u_sh->mouse_button);
            lua_pushboolean(L, u_sh->mouse_down);
            lua_pushinteger(L, u_sh->mouse_wheel);
            lua_pushinteger(L, u_sh->mouse_buttons); n = 7;
        }
        lua_call(L, n, 0);
    }
    painting = 0;
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
