/* Built-in demonstrations are native C apps. Editable Lua belongs to projects. */
#include "scos.h"
struct counter {u32 value;int hover;};
static void counter_open(struct window *w,void *arg){(void)arg;struct counter *d=palloc(sizeof(*d));if(!d)return;memset(d,0,sizeof(*d));u32 n;char *p=vfs_read("home/appdata/counter/count.txt",&n);if(p&&n<12)d->value=str_to_u32(p);if(d->value>999999999)d->value=0;w->data=d;}
static void counter_close(struct window *w){if(w->data)pfree(w->data,sizeof(struct counter));}
static void counter_add(struct window *w){struct counter *d=w->data;if(d->value<999999999)d->value++;wm_redraw(w);}
static void counter_save(struct window *w){struct counter *d=w->data;char n[16];fmt_u32(n,d->value);vfs_mkdir("home/appdata");vfs_mkdir("home/appdata/counter");int ok=vfs_write("home/appdata/counter/count.txt",n,strlen(n));wm_dialog("Counter",ok?"Saved in RAM. Terminal save persists only on supported ATA disks.":"Save failed",NULL,NULL,NULL);}
static void counter_paint(struct window *w){struct counter *d=w->data;struct surface *s=&w->surf;const struct theme *t=theme_current();s_fill(s,0,0,s->w,s->h,t->win_bg);s_text(s,20,20,"Counter - native C",t->main);char n[32];strcpy(n,"Count: ");fmt_u32(n+7,d->value);s_text(s,20,52,n,t->text);int width=s->w-40;if(width>260)width=260;s_fill(s,20,84,width,40,d->hover?t->main:color_blend(t->win_bg,t->main,20));s_frame_rect(s,20,84,width,40,t->main);s_clip_text(s,30,96,"Click or press Space",d->hover?t->title_text:t->text,width-20);if(s->h>=170)s_text(s,20,148,"S saves count in RAM",t->text);}
static void counter_key(struct window *w,struct key_event *e){if(!e->pressed)return;if(e->keycode==' ')counter_add(w);if(e->keycode=='s'||e->keycode=='S')counter_save(w);}
static void counter_mouse(struct window *w,struct mouse_event *e,int x,int y){struct counter *d=w->data;int width=w->surf.w-40;if(width>260)width=260;int hit=x>=20&&x<20+width&&y>=84&&y<124;if(hit!=d->hover){d->hover=hit;wm_redraw(w);}if(hit&&e->type==MEV_BUTTON&&e->down&&e->button==MBTN_LEFT)counter_add(w);}
struct app app_counter={.id="counter",.title="Counter",.icon=ICON_INFO,.def_w=560,.def_h=360,.uses_data=1,.open=counter_open,.close=counter_close,.paint=counter_paint,.key=counter_key,.mouse=counter_mouse};
SCOS_APP(app_counter,010);
struct mark {u16 x,y;};
struct sketch {int count;struct mark marks[1000];};
static void sketch_open(struct window *w,void *arg){(void)arg;w->data=palloc(sizeof(struct sketch));if(w->data)memset(w->data,0,sizeof(struct sketch));}
static void sketch_close(struct window *w){if(w->data)pfree(w->data,sizeof(struct sketch));}
static void sketch_paint(struct window *w){struct sketch *d=w->data;struct surface *s=&w->surf;const struct theme *t=theme_current();s_fill(s,0,0,s->w,s->h,t->win_bg);s_clip_text(s,16,16,"Sketch - click to draw; R clears",t->main,s->w-32);for(int i=0;i<d->count;i++)s_fill(s,(u32)d->marks[i].x*s->w/65535,(u32)d->marks[i].y*s->h/65535,6,6,color_blend(t->main,t->text,(i*37)%70));char n[32];fmt_u32(n,d->count);strcat(n," marks");s_text(s,16,s->h-24,n,t->text);}
static void sketch_mouse(struct window *w,struct mouse_event *e,int x,int y){struct sketch *d=w->data;if(e->type==MEV_BUTTON&&e->down&&e->button==MBTN_LEFT&&d->count<1000&&x>=0&&y>=40&&x<w->surf.w&&y<w->surf.h-30){d->marks[d->count++]=(struct mark){(u32)x*65535/w->surf.w,(u32)y*65535/w->surf.h};wm_redraw(w);}}
static void sketch_key(struct window *w,struct key_event *e){if(e->pressed&&(e->keycode=='r'||e->keycode=='R')){((struct sketch *)w->data)->count=0;wm_redraw(w);}}
struct app app_sketch={.id="sketch",.title="Sketch",.icon=ICON_NOTEPAD,.def_w=560,.def_h=360,.uses_data=1,.open=sketch_open,.close=sketch_close,.paint=sketch_paint,.mouse=sketch_mouse,.key=sketch_key};
SCOS_APP(app_sketch,011);
