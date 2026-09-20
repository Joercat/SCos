/* Lua apps are file-backed clients of the ordinary window/app interface.
 * This is a bounded language runtime, NOT a ring-3 security boundary. */
#include "scos.h"
#include "cat.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define SCRIPT_MAX 65536u
#define ARENA_SIZE (2u*1024u*1024u)
#define LUA_APPS 32
struct lua_app { struct app app; char id[32], title[40], path[192]; };
static struct lua_app scripts[LUA_APPS];
static int script_count;
struct block { size_t size; struct block *next; int free; u32 pad; };
struct lua_window {
    lua_State *L;
    struct window *w;
    void *arena;
    int ref, failed, painting, budget;
    u32 deadline, next_tick, pixels;
    char error[224];
    u32 permissions;
    int mouse_x,mouse_y,mouse_buttons;
    int interval;
    int theme_used,message_used;u32 theme_at,message_at;
};
static struct lua_window *context(lua_State *L) {
    return *(struct lua_window **)lua_getextraspace(L);
}
static void *allocate(void *ud,void *ptr,size_t old,size_t size) {
    struct lua_window *d=ud;
    struct block *b=ptr?(struct block *)ptr-1:NULL;
    if (!size) {
        if(b)b->free=1;
        for(struct block *p=d->arena;p&&p->next;) {
            if(p->free&&p->next->free){p->size+=sizeof(*p)+p->next->size;p->next=p->next->next;}
            else p=p->next;
        }
        return NULL;
    }
    if(size>ARENA_SIZE-sizeof(struct block))return NULL;
    size=(size+15)&~(size_t)15;
    if(b&&size<=b->size)return ptr;
    for(struct block *p=d->arena;p;p=p->next)if(p->free&&p->size>=size){
        if(p->size>=size+sizeof(*p)+16){
            struct block *tail=(void *)((char *)(p+1)+size);
            *tail=(struct block){p->size-size-sizeof(*p),p->next,1,0};p->next=tail;p->size=size;
        }
        p->free=0;
        if(ptr){memcpy(p+1,ptr,old<size?old:size);allocate(ud,ptr,old,0);}
        return p+1;
    }
    return NULL;
}
/* Also called from the upstream pattern matcher; hooks alone cannot bound C. */
void scos_lua_checkbudget(lua_State *L) {
    struct lua_window *d=context(L);
    d->budget--;
    if(d->budget<=0 || (i32)((u32)tick_count-d->deadline)>=0)
        luaL_error(L,"app callback exceeded its execution budget");
}
void scos_lua_charge(lua_State *L, size_t work) {
    struct lua_window *d=context(L);
    if(work>(size_t)d->budget)luaL_error(L,"app callback exceeded its execution budget");
    d->budget-=(int)work;scos_lua_checkbudget(L);
}
static void hook(lua_State *L,lua_Debug *ar){(void)ar;context(L)->budget-=99;scos_lua_checkbudget(L);}
static void budget(struct lua_window *d) {
    d->budget=100000;d->deadline=(u32)tick_count+10;d->pixels=4000000;
    lua_sethook(d->L,hook,LUA_MASKCOUNT,100);
}
static void fail(struct lua_window *d,const char *s) {
    strncpy(d->error,s?s:"Lua error",sizeof(d->error)-1);d->error[sizeof(d->error)-1]=0;
    d->failed=1;d->w->dirty=1;app_log(d->w,d->error);
}
static int integer(lua_State *L,int n,int lo,int hi) {
    lua_Integer v=luaL_checkinteger(L,n);
    if(v<lo||v>hi)luaL_error(L,"argument %d outside allowed range",n);
    return (int)v;
}
static u32 color(lua_State *L,int n){return (u32)integer(L,n,0,0xffffff);}
static struct surface *canvas(lua_State *L,u32 cost) {
    struct lua_window *d=context(L);
    if(!d->painting)luaL_error(L,"drawing is only allowed in paint");
    if(cost>d->pixels)luaL_error(L,"drawing budget exceeded");
    d->pixels-=cost;return &d->w->surf;
}
static int api_clear(lua_State *L) {
    struct window *w=context(L)->w;u32 c=color(L,1);
    struct surface *s=canvas(L,(u32)w->surf.w*w->surf.h);s_fill(s,0,0,s->w,s->h,c);return 0;
}
static int api_rect(lua_State *L) {
    int x=integer(L,1,-4096,4096),y=integer(L,2,-4096,4096);
    int w=integer(L,3,0,4096),h=integer(L,4,0,4096);u32 c=color(L,5);
    s_fill(canvas(L,(u32)w*h),x,y,w,h,c);return 0;
}
static int api_text(lua_State *L) {
    int x=integer(L,1,-4096,4096),y=integer(L,2,-4096,4096);size_t n;
    const char *s=luaL_checklstring(L,3,&n);u32 c=color(L,4);
    if(n>1024) return luaL_error(L,"text exceeds 1024 bytes");
    s_text(canvas(L,(u32)n*128),x,y,s,c);return 0;
}
static int api_size(lua_State *L) {struct window *w=context(L)->w;lua_pushinteger(L,w->surf.w);lua_pushinteger(L,w->surf.h);return 2;}
static int api_redraw(lua_State *L){wm_redraw(context(L)->w);return 0;}
static int api_title(lua_State *L){
    size_t n;const char *s=luaL_checklstring(L,1,&n);
    if(n>63)return luaL_error(L,"title exceeds 63 bytes");
    struct window *w=context(L)->w;wm_set_title(w,s);return 0;
}
static int api_log(lua_State *L){size_t n;const char *s=luaL_checklstring(L,1,&n);if(n>512)return luaL_error(L,"log exceeds 512 bytes");app_log(context(L)->w,s);return 0;}
static int api_clock(lua_State *L){lua_pushnumber(L,(lua_Number)tick_count/100.0);return 1;}
static void data_path(lua_State *L,char *out) {
    size_t n;const char *name=luaL_checklstring(L,1,&n);
    if(strlen(name)!=n)luaL_error(L,"invalid data filename");
    if(!n||n>=VFS_NAME||!strcmp(name,".")||!strcmp(name,".."))luaL_error(L,"invalid data filename");
    for(size_t i=0;i<n;i++)if(!((name[i]>='a'&&name[i]<='z')||(name[i]>='A'&&name[i]<='Z')||(name[i]>='0'&&name[i]<='9')||name[i]=='.'||name[i]=='_'||name[i]=='-'))luaL_error(L,"invalid data filename");
    strcpy(out,"home/appdata/");strcat(out,context(L)->w->app->id);
    vfs_mkdir("home/appdata");vfs_mkdir(out);strcat(out,"/");strcat(out,name);
}
static int api_read(lua_State *L){char path[128];data_path(L,path);u32 n;char *s=vfs_read(path,&n);if(!s)lua_pushnil(L);else if(n>SCRIPT_MAX)return luaL_error(L,"data file too large");else lua_pushlstring(L,s,n);return 1;}
static int api_write(lua_State *L){
    char path[128];data_path(L,path);size_t n;const char *s=luaL_checklstring(L,2,&n);
    if(n>SCRIPT_MAX)return luaL_error(L,"data file too large");
    struct vfs_node *old=vfs_lookup(path);
    char dirpath[128];strcpy(dirpath,"home/appdata/");strcat(dirpath,context(L)->w->app->id);
    struct vfs_node *dir=vfs_lookup(dirpath);u64 bytes=0;int files=0;
    if(dir)for(struct vfs_node *f=dir->child;f;f=f->sibling){bytes+=f->size;files++;}
    if((!old&&files>=16)||bytes-(old?old->size:0)+n>SCRIPT_MAX)
        return luaL_error(L,"app data quota exceeded (16 files / 64 KiB)");
    lua_pushboolean(L,vfs_write(path,s,(u32)n));return 1;
}
#include "lua_api.inc"
static const luaL_Reg api[]={
    {"clear",api_clear},{"rect",api_rect},{"text",api_text},{"size",api_size},
    {"redraw",api_redraw},{"title",api_title},{"log",api_log},{"time",api_clock},
    {"read",api_read},{"write",api_write},
    {"pixel",api_pixel},{"frame",api_frame},{"line",api_line},{"circle",api_circle},{"disc",api_disc},
    {"gradient",api_gradient},{"icon",api_icon},{"text_width",api_text_width},{"text_scaled",api_text_scaled},
    {"rgb",api_rgb},{"blend",api_blend},{"clamp",api_clamp},{"hit",api_hit},
    {"button",api_button},{"checkbox",api_checkbox},{"progress",api_progress},
    {"resize",api_resize},{"move",api_move},{"window",api_window},{"screen",api_screen},{"mouse",api_mouse_state},{"interval",api_interval},
    {"app_id",api_app_id},{"memory",api_memory},{"date",api_date},{"uptime",api_clock},
    {"exists",api_exists},{"file_size",api_file_size},{"remove",api_remove},{"rename",api_rename},{"files",api_files},
    {"theme",api_theme},{"themes",api_themes},{"theme_apply",api_theme_apply},{"theme_custom",api_theme_custom},
    {"theme_status",api_theme_status},{"message",api_message},{"api_info",api_info},{NULL,NULL}
};
static int initialize(lua_State *L) {
    luaL_requiref(L,"_G",luaopen_base,1);lua_pop(L,1);
    luaL_requiref(L,"table",luaopen_table,1);lua_pop(L,1);
    luaL_requiref(L,"string",luaopen_string,1);lua_pop(L,1);
    /* Bytecode dumping would invite loading unvalidated kernel VM input. */
    lua_getglobal(L,"string");lua_pushnil(L);lua_setfield(L,-2,"dump");lua_pop(L,1);
    luaL_requiref(L,"math",luaopen_math,1);lua_pop(L,1);
    luaL_requiref(L,"utf8",luaopen_utf8,1);lua_pop(L,1);
    luaL_newlib(L,api);lua_pushinteger(L,2);lua_setfield(L,-2,"version");lua_setglobal(L,"scos");return 0;
}
/* Lookup and argument construction occur INSIDE the protected call too:
 * even a table lookup/metamethod or allocating an event can raise an error. */
struct invocation { const char *name; struct mouse_event *mouse; struct key_event *key; int x,y; };
static int dispatch(lua_State *L) {
    struct lua_window *d=context(L);struct invocation *i=lua_touserdata(L,1);
    lua_rawgeti(L,LUA_REGISTRYINDEX,d->ref);
    lua_pushstring(L,i->name);lua_rawget(L,-2);
    if(lua_isnil(L,-1))return 0;
    if(!lua_isfunction(L,-1))return luaL_error(L,"callback must be a function");
    int n=0;
    if(i->mouse){struct mouse_event *e=i->mouse;
        lua_pushinteger(L,e->type);lua_pushinteger(L,i->x);lua_pushinteger(L,i->y);
        lua_pushinteger(L,e->button);lua_pushboolean(L,e->down);lua_pushinteger(L,e->wheel);lua_pushinteger(L,e->buttons);n=7;
    }else if(i->key){struct key_event *e=i->key;
        lua_pushinteger(L,e->keycode);lua_pushboolean(L,e->pressed);lua_pushboolean(L,e->shift);lua_pushboolean(L,e->ctrl);lua_pushboolean(L,e->alt);n=5;
    }else if(!strcmp(i->name,"paint")){lua_pushinteger(L,d->w->surf.w);lua_pushinteger(L,d->w->surf.h);n=2;}
    else if(!strcmp(i->name,"tick")){lua_pushnumber(L,(lua_Number)tick_count/100.0);n=1;}
    lua_call(L,n,0);return 0;
}
static void invoke(struct lua_window *d,struct invocation *i) {
    if(d->failed)return;
    budget(d);lua_pushcfunction(d->L,dispatch);lua_pushlightuserdata(d->L,i);
    if(lua_pcall(d->L,1,0,0)!=LUA_OK){fail(d,lua_type(d->L,-1)==LUA_TSTRING?lua_tostring(d->L,-1):"non-string Lua error");lua_pop(d->L,1);}
}
static int load_app(lua_State *L) {
    struct lua_window *d=context(L);struct lua_app *a=(struct lua_app *)d->w->app;
    u32 n=0;char *s=vfs_read(a->path,&n);struct cat_info m;char err[160];
    if(!cat_validate(s,n,&m,err,sizeof(err)))return luaL_error(L,"%s",err);
    if(strcmp(m.id,a->id))return luaL_error(L,"Package identity changed; use a new path and restart");
    d->permissions=m.permissions;
    if(luaL_loadbufferx(L,m.source,m.length,a->path,"t")!=LUA_OK)return lua_error(L);
    lua_call(L,0,1);
    if(!lua_istable(L,-1))return luaL_error(L,"app source must return a callback table");
    d->ref=luaL_ref(L,LUA_REGISTRYINDEX);return 0;
}
static void script_open(struct window *w,void *arg) {
    (void)arg;struct lua_window *d=palloc(sizeof(*d));if(!d)return;
    memset(d,0,sizeof(*d));w->data=d;d->w=w;d->ref=LUA_NOREF;d->interval=10;
    d->arena=palloc(ARENA_SIZE);if(!d->arena){fail(d,"Not enough memory for Lua arena");return;}
    *(struct block *)d->arena=(struct block){ARENA_SIZE-sizeof(struct block),NULL,1,0};
    d->L=lua_newstate(allocate,d);if(!d->L){fail(d,"Unable to create Lua state");return;}
    *(struct lua_window **)lua_getextraspace(d->L)=d;budget(d);
    lua_pushcfunction(d->L,initialize);
    if(lua_pcall(d->L,0,0,0)!=LUA_OK){fail(d,lua_type(d->L,-1)==LUA_TSTRING?lua_tostring(d->L,-1):"non-string Lua error");lua_pop(d->L,1);return;}
    lua_pushcfunction(d->L,load_app);
    if(lua_pcall(d->L,0,0,0)!=LUA_OK){fail(d,lua_type(d->L,-1)==LUA_TSTRING?lua_tostring(d->L,-1):"non-string Lua error");lua_pop(d->L,1);return;}
    struct invocation i={.name="open"};invoke(d,&i);
}
static void script_paint(struct window *w){
    struct lua_window *d=w->data;if(!d)return;
    s_fill(&w->surf,0,0,w->surf.w,w->surf.h,0x101820);
    if(!d->failed){d->painting=1;struct invocation i={.name="paint"};invoke(d,&i);d->painting=0;}
    if(d->failed){s_text(&w->surf,12,12,"Lua app stopped",0xff7777);s_clip_text(&w->surf,12,36,d->error,0xffffff,w->surf.w-24);s_text(&w->surf,12,60,"Close, fix the source, then launch again.",0xcccccc);}
}
static void script_key(struct window *w,struct key_event *e){struct invocation i={.name="key",.key=e};invoke(w->data,&i);}
static void script_mouse(struct window *w,struct mouse_event *e,int x,int y){struct lua_window *d=w->data;d->mouse_x=x;d->mouse_y=y;d->mouse_buttons=e->buttons;struct invocation i={.name="mouse",.mouse=e,.x=x,.y=y};invoke(w->data,&i);}
static void script_tick(struct window *w){struct lua_window *d=w->data;if((i32)((u32)tick_count-d->next_tick)<0)return;d->next_tick=(u32)tick_count+(u32)d->interval;struct invocation i={.name="tick"};invoke(d,&i);}
static void script_close(struct window *w){
    struct lua_window *d=w->data;if(!d)return;
    /* The arena owns everything, and the API creates no external handles.
     * Do not execute user __gc/__close code while destroying a window. */
    if(d->arena)pfree(d->arena,ARENA_SIZE);
    pfree(d,sizeof(*d));w->data=NULL;
}
static const char *script_failure(struct window *w) {
    struct lua_window *d=w->data;return d&&d->failed?d->error:NULL;
}
int lua_source_check(const char *source,u32 length,char *err,size_t cap) {
    if(!source||!length||length>CAT_SOURCE_MAX){if(cap){strncpy(err,"Source must be 1..65536 bytes",cap-1);err[cap-1]=0;}return 0;}
    struct lua_window d;memset(&d,0,sizeof(d));d.arena=palloc(ARENA_SIZE);
    if(!d.arena){if(cap){strncpy(err,"Not enough memory to compile",cap-1);err[cap-1]=0;}return 0;}
    *(struct block *)d.arena=(struct block){ARENA_SIZE-sizeof(struct block),NULL,1,0};
    d.L=lua_newstate(allocate,&d);int ok=0;
    if(d.L)ok=luaL_loadbufferx(d.L,source,length,"project","t")==LUA_OK;
    if(cap){const char *message=ok?"Syntax check passed":d.L?lua_tostring(d.L,-1):"Cannot create compiler state";strncpy(err,message?message:"Compile failed",cap-1);err[cap-1]=0;}
    pfree(d.arena,ARENA_SIZE);return ok;
}
struct app *lua_app_install(const char *path) {
    if(!path||strlen(path)>=sizeof(scripts[0].path))return NULL;
    if(*path=='/')path++;
    size_t n=strlen(path);if(n<5||strcmp(path+n-4,".cat"))return NULL;
    u32 length=0;char *bytes=vfs_read(path,&length);struct cat_info m;char err[128];
    if(!cat_validate(bytes,length,&m,err,sizeof(err)))return NULL;
    for(int i=0;i<script_count;i++)if(!strcmp(path,scripts[i].path)) {
        if(strcmp(m.id,scripts[i].id))return NULL;
        strcpy(scripts[i].title,m.title);scripts[i].app.def_w=m.width;scripts[i].app.def_h=m.height;
        return &scripts[i].app;
    }
    if(script_count>=LUA_APPS)return NULL;
    for(int i=0;i<app_count();i++)if(!strcmp(app_at(i)->id,m.id))return NULL;
    struct lua_app *a=&scripts[script_count];strcpy(a->id,m.id);strcpy(a->title,m.title);strcpy(a->path,path);
    a->app=(struct app){.id=a->id,.title=a->title,.icon=ICON_NOTEPAD,.def_w=m.width,.def_h=m.height,.uses_data=1,
        .open=script_open,.paint=script_paint,.key=script_key,.mouse=script_mouse,.tick=script_tick,.close=script_close,.failure=script_failure,.external=1};
    if(!app_register(&a->app))return NULL;
    script_count++;return &a->app;
}
void lua_apps_refresh(void) {
    struct vfs_node *dir=vfs_lookup("home/apps");if(!dir||!dir->is_dir)return;
    for(struct vfs_node *n=dir->child;n;n=n->sibling)if(!n->is_dir){char p[192];strcpy(p,"home/apps/");strcat(p,n->name);lua_app_install(p);}
}
