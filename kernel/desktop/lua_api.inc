/* API v2 implementation; included with the per-window runtime context. */
static int xy(lua_State *L,int i){return integer(L,i,-4096,4096);}
static int extent(lua_State *L,int i){return integer(L,i,0,4096);}
static const char *label(lua_State *L,int i){size_t n;const char *s=luaL_checklstring(L,i,&n);if(n>1024)luaL_error(L,"text exceeds 1024 bytes");return s;}
static int api_pixel(lua_State *L){int x=xy(L,1),y=xy(L,2);u32 c=color(L,3);s_pixel(canvas(L,1),x,y,c);return 0;}
static int api_frame(lua_State *L){int x=xy(L,1),y=xy(L,2),w=extent(L,3),h=extent(L,4);u32 c=color(L,5);if(w&&h)s_frame_rect(canvas(L,2*(u32)(w+h)),x,y,w,h,c);return 0;}
static int api_line(lua_State *L){int x=xy(L,1),y=xy(L,2),xx=xy(L,3),yy=xy(L,4);u32 c=color(L,5);s_line(canvas(L,16384),x,y,xx,yy,c);return 0;}
static int api_circle(lua_State *L){int x=xy(L,1),y=xy(L,2),r=integer(L,3,0,1024);u32 c=color(L,4);s_circle(canvas(L,8*(u32)r+1),x,y,r,c);return 0;}
static int api_disc(lua_State *L){int x=xy(L,1),y=xy(L,2),r=integer(L,3,0,1024);u32 c=color(L,4);s_disc(canvas(L,4*(u32)r*r+1),x,y,r,c);return 0;}
static int api_gradient(lua_State *L){int x=xy(L,1),y=xy(L,2),w=extent(L,3),h=extent(L,4);u32 a=color(L,5),b=color(L,6);if(w&&h)s_vgrad(canvas(L,(u32)w*h),x,y,w,h,a,b);return 0;}
static int api_icon(lua_State *L){int id=integer(L,1,0,ICON_COUNT-1),x=xy(L,2),y=xy(L,3);u32 c=color(L,4);s_icon(canvas(L,4096),id,x,y,c);return 0;}
static int api_text_width(lua_State *L){lua_pushinteger(L,s_text_width(label(L,1)));return 1;}
static int api_text_scaled(lua_State *L){int x=xy(L,1),y=xy(L,2);const char *s=label(L,3);u32 c=color(L,4);int scale=integer(L,5,1,4);s_text_scaled(canvas(L,(u32)strlen(s)*128*scale*scale),x,y,s,c,scale);return 0;}
static int api_rgb(lua_State *L){int r=integer(L,1,0,255),g=integer(L,2,0,255),b=integer(L,3,0,255);lua_pushinteger(L,(r<<16)|(g<<8)|b);return 1;}
static int api_blend(lua_State *L){u32 a=color(L,1),b=color(L,2);int p=integer(L,3,0,100);lua_pushinteger(L,color_blend(a,b,p*255/100));return 1;}
static int api_clamp(lua_State *L){lua_Number x=luaL_checknumber(L,1),lo=luaL_checknumber(L,2),hi=luaL_checknumber(L,3);if(lo>hi)return luaL_error(L,"clamp bounds reversed");lua_pushnumber(L,x<lo?lo:x>hi?hi:x);return 1;}
static int api_hit(lua_State *L){int x=xy(L,1),y=xy(L,2),rx=xy(L,3),ry=xy(L,4),w=extent(L,5),h=extent(L,6);lua_pushboolean(L,x>=rx&&y>=ry&&x<rx+w&&y<ry+h);return 1;}
static int api_button(lua_State *L){int x=xy(L,1),y=xy(L,2),w=extent(L,3),h=extent(L,4);const char *s=label(L,5);int active=lua_toboolean(L,6);const struct theme *t=theme_current();struct surface *c=canvas(L,(u32)w*h+(u32)strlen(s)*128+2*(u32)(w+h));if(w&&h){s_fill(c,x,y,w,h,active?t->main:color_blend(t->win_bg,t->main,18));s_frame_rect(c,x,y,w,h,t->main);if(h>=FONT_H)s_clip_text(c,x+8,y+(h-FONT_H)/2,s,active?t->title_text:t->text,w>16?w-16:0);}return 0;}
static int api_checkbox(lua_State *L){int x=xy(L,1),y=xy(L,2),checked=lua_toboolean(L,4);const char *s=label(L,3);const struct theme *t=theme_current();struct surface *c=canvas(L,2048+(u32)strlen(s)*128);s_frame_rect(c,x,y,16,16,t->main);if(checked)s_fill(c,x+4,y+4,8,8,t->main);s_text(c,x+24,y,s,t->text);return 0;}
static int api_progress(lua_State *L){int x=xy(L,1),y=xy(L,2),w=extent(L,3),h=extent(L,4),p=integer(L,5,0,100);const struct theme *t=theme_current();struct surface *c=canvas(L,2*(u32)w*h+2*(u32)(w+h));if(w&&h){s_fill(c,x,y,w,h,t->win_bg);s_fill(c,x,y,w*p/100,h,t->main);s_frame_rect(c,x,y,w,h,t->text);}return 0;}
static void field(lua_State *L,const char *s,lua_Integer n){lua_pushinteger(L,n);lua_setfield(L,-2,s);}
static int api_window(lua_State *L){struct window *w=context(L)->w;lua_newtable(L);field(L,"x",w->x);field(L,"y",w->y);field(L,"width",w->w);field(L,"height",w->h);field(L,"state",w->state);lua_pushboolean(L,wm_focused()==w);lua_setfield(L,-2,"focused");return 1;}
static int api_screen(lua_State *L){lua_pushinteger(L,screen_w);lua_pushinteger(L,screen_h);return 2;}
static int api_mouse_state(lua_State *L){struct lua_window *d=context(L);lua_pushinteger(L,d->mouse_x);lua_pushinteger(L,d->mouse_y);lua_pushinteger(L,wm_focused()==d->w?d->mouse_buttons:0);return 3;}
static int api_interval(lua_State *L){int ms=integer(L,1,100,60000);context(L)->interval=(ms+9)/10;return 0;}
static int api_app_id(lua_State *L){lua_pushstring(L,context(L)->w->app->id);return 1;}
static int api_memory(lua_State *L){lua_pushinteger(L,(lua_Integer)heap_owner_bytes((uintptr_t)context(L)->w));lua_pushinteger(L,ARENA_SIZE);return 2;}
static int api_date(lua_State *L){struct rtc_time t;rtc_read(&t);lua_newtable(L);field(L,"year",t.year);field(L,"month",t.mon);field(L,"day",t.day);field(L,"hour",t.hour);field(L,"minute",t.min);field(L,"second",t.sec);field(L,"weekday",t.weekday);return 1;}
static int api_exists(lua_State *L){char p[128];data_path(L,p);struct vfs_node *n=vfs_lookup(p);lua_pushboolean(L,n&&!n->is_dir);return 1;}
static int api_file_size(lua_State *L){char p[128];data_path(L,p);struct vfs_node *n=vfs_lookup(p);if(n&&!n->is_dir)lua_pushinteger(L,n->size);else lua_pushnil(L);return 1;}
static int api_remove(lua_State *L){char p[128];data_path(L,p);lua_pushboolean(L,vfs_delete(p));return 1;}
static int api_rename(lua_State *L){char a[128],b[128];data_path(L,a);lua_pushvalue(L,2);lua_replace(L,1);data_path(L,b);lua_pushboolean(L,vfs_rename(a,b));return 1;}
static int api_files(lua_State *L){char p[96];strcpy(p,"home/appdata/");strcat(p,context(L)->w->app->id);struct vfs_node *dir=vfs_lookup(p);lua_newtable(L);int i=1;if(dir&&dir->is_dir)for(struct vfs_node *n=dir->child;n&&i<=256;n=n->sibling)if(!n->is_dir){lua_pushstring(L,n->name);lua_rawseti(L,-2,i++);}return 1;}
static const char *theme_fields[]={"main","bg_top","bg_bot","win_bg","text","title_text","taskbar_bg","mode","spacing","grid"};
static int api_theme(lua_State *L){const struct theme *t=theme_current();u32 values[10];theme_values(t,values);lua_newtable(L);lua_pushstring(L,t->id);lua_setfield(L,-2,"id");for(int i=0;i<10;i++)field(L,theme_fields[i],values[i]);return 1;}
static int api_themes(lua_State *L){lua_newtable(L);for(int i=0;i<theme_count();i++){lua_pushstring(L,theme_get(i)->id);lua_rawseti(L,-2,i+1);}return 1;}
static struct {int pending,status,custom;char id[32];u32 colors[10];} theme_request;
static void theme_answer(int ok,const char *text,void *ud){(void)text;(void)ud;theme_request.pending=0;theme_request.status=2;if(!ok)return;if(theme_request.custom&&!theme_custom_store(theme_request.id,theme_request.colors,1)){theme_request.status=3;return;}int i=theme_index_of_id(theme_request.id);if(i<0){theme_request.status=3;return;}theme_set_index(i);settings_save();wm_theme_changed();theme_request.status=1;}
static int request_theme(lua_State *L,const char *id,const u32 *colors){struct lua_window *d=context(L);if(!(d->permissions&CAT_PERMISSION_THEME))return luaL_error(L,"CAT package does not request theme permission");if(d->painting)return luaL_error(L,"theme changes cannot be requested from paint");if(theme_request.pending||wm_dialog_active()||(d->theme_used&&(u32)(tick_count-d->theme_at)<500)){lua_pushboolean(L,0);return 1;}d->theme_used=1;d->theme_at=tick_count;strcpy(theme_request.id,id);theme_request.custom=colors!=NULL;if(colors)memcpy(theme_request.colors,colors,40);char msg[256];strcpy(msg,"App: ");strcat(msg,d->w->app->id);strcat(msg,"\nChange SYSTEM theme to:\n");strcat(msg,id);strcat(msg,"\nApply and save in RAM?");theme_request.pending=1;theme_request.status=0;wm_dialog("Theme permission",msg,NULL,theme_answer,NULL);if(!wm_dialog_active()){theme_request.pending=0;theme_request.status=3;}lua_pushboolean(L,theme_request.pending);return 1;}
static int api_theme_apply(lua_State *L){const char *id=luaL_checkstring(L,1);if(strlen(id)>30||theme_index_of_id(id)<0)return luaL_error(L,"unknown theme");return request_theme(L,id,NULL);}
static int api_theme_custom(lua_State *L){const char *id=luaL_checkstring(L,1);if(!theme_id_valid(id))return luaL_error(L,"custom ID must be user- plus lowercase letters/digits/dashes, max 30");luaL_checktype(L,2,LUA_TTABLE);u32 v[10];theme_values(theme_current(),v);for(int i=0;i<10;i++){lua_getfield(L,2,theme_fields[i]);if(!lua_isnil(L,-1)){lua_Integer n=luaL_checkinteger(L,-1);if(n<0||n>(i==7?3:i==8?256:0xffffff)||(i==8&&n<8))return luaL_error(L,"invalid theme field %s",theme_fields[i]);v[i]=n;}lua_pop(L,1);}return request_theme(L,id,v);}
static int api_theme_status(lua_State *L){lua_pushstring(L,theme_request.pending?"pending":theme_request.status==1?"applied":theme_request.status==2?"denied":theme_request.status==3?"failed":"idle");return 1;}
static int api_message(lua_State *L){struct lua_window *d=context(L);const char *s=label(L,1);if(strlen(s)>192)return luaL_error(L,"message exceeds 192 bytes");if(d->painting)return luaL_error(L,"messages cannot be requested from paint");if(wm_dialog_active()||(d->message_used&&(u32)(tick_count-d->message_at)<500)){lua_pushboolean(L,0);return 1;}d->message_used=1;d->message_at=tick_count;wm_dialog(d->w->app->title,s,NULL,NULL,NULL);lua_pushboolean(L,wm_dialog_active());return 1;}
static int api_info(lua_State *L){lua_newtable(L);field(L,"version",2);field(L,"source_limit",CAT_SOURCE_MAX);field(L,"arena_limit",ARENA_SIZE);field(L,"data_limit",SCRIPT_MAX);field(L,"data_files",16);field(L,"theme_permission",!!(context(L)->permissions&CAT_PERMISSION_THEME));return 1;}

static int api_resize(lua_State *L){struct lua_window *d=context(L);if(d->painting)return luaL_error(L,"resize is not allowed in paint");int w=integer(L,1,320,2048),h=integer(L,2,200,2048);scos_lua_charge(L,(size_t)w*h/16+1);lua_pushboolean(L,wm_resize_window(d->w,w,h));return 1;}
static int api_move(lua_State *L){struct lua_window *d=context(L);if(d->painting)return luaL_error(L,"move is not allowed in paint");int x=xy(L,1),y=xy(L,2);lua_pushboolean(L,wm_move_window(d->w,x,y));return 1;}

/* Icons are always native 24x24 pixels; scripts cannot request a scaled icon. */
static const char *icon_names[]={"folder","terminal","notepad","browser","calendar","settings","info","cards","chart","solitaire"};
static int api_icon_size(lua_State *L){lua_pushinteger(L,24);lua_pushinteger(L,24);return 2;}
static int api_icons(lua_State *L){lua_newtable(L);for(int i=0;i<ICON_COUNT;i++)field(L,icon_names[i],i);return 1;}
static int api_set_icon(lua_State *L){struct lua_window *d=context(L);if(d->painting)return luaL_error(L,"set_icon is not allowed in paint");int id=integer(L,1,0,ICON_COUNT-1);if(d->w->app->icon!=id){d->w->app->icon=id;wm_request_full();}return 0;}
static int api_text_clip(lua_State *L){int x=xy(L,1),y=xy(L,2),width=extent(L,3);const char *s=label(L,4);u32 c=color(L,5);s_clip_text(canvas(L,(u32)strlen(s)*128),x,y,s,c,width);return 0;}
static int api_text_wrap(lua_State *L){
    int x=xy(L,1),y=xy(L,2),width=extent(L,3),height=extent(L,4);const char *s=label(L,5);u32 c=color(L,6);
    struct surface *surf=canvas(L,(u32)strlen(s)*128);int cols=width/8,rows=height/16,used=0;if(cols>127)cols=127;
    while(*s&&cols>0&&used<rows){char line[128];int n=0;while(*s&&*s!='\n'&&n<cols){char ch=*s++;if(ch!='\r')line[n++]=ch;}if(*s=='\n')s++;line[n]=0;s_text(surf,x,y+used*16,line,c);used++;}
    lua_pushinteger(L,used);lua_pushboolean(L,!*s);return 2;
}
static int api_focused(lua_State *L){lua_pushboolean(L,wm_focused()==context(L)->w);return 1;}
static int api_minimize(lua_State *L){struct lua_window *d=context(L);if(d->painting)return luaL_error(L,"minimize is not allowed in paint");wm_minimize_window(d->w);return 0;}

static int api_notify(lua_State *L){
    struct lua_window *d=context(L);const char *text=label(L,1);
    if(strlen(text)>192)return luaL_error(L,"notification exceeds 192 bytes");
    if(d->painting)return luaL_error(L,"notifications cannot be requested in paint");
    if(d->message_used&&(u32)(tick_count-d->message_at)<500){lua_pushboolean(L,0);return 1;}
    d->message_used=1;d->message_at=tick_count;wm_notify(d->w->app->id,text,0);lua_pushboolean(L,1);return 1;
}
