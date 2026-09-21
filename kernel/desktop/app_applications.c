/* Native application manager; built-ins are visible but never removable. */
#include "scos.h"
struct manager {char selected[32];int scroll;};
static struct app *entry(int n){for(int i=0;i<app_count();i++){struct app *a=app_at(i);if(a->id[0]=='_')continue;if(!n--)return a;}return NULL;}
static int entries(void){int n=0;for(int i=0;i<app_count();i++)if(app_at(i)->id[0]!='_')n++;return n;}
static void am_open(struct window *w,void *arg){(void)arg;w->data=palloc(sizeof(struct manager));if(w->data)memset(w->data,0,sizeof(struct manager));}
static void am_close(struct window *w){if(w->data)pfree(w->data,sizeof(struct manager));}
static void am_paint(struct window *w){
    struct manager *d=w->data;struct surface *s=&w->surf;const struct theme *t=theme_current();s_fill(s,0,0,s->w,s->h,t->win_bg);
    const char *buttons[]={"Launch","Pin / Unpin","Uninstall"};for(int i=0;i<3;i++){s_frame_rect(s,12+i*116,10,108,28,t->main);s_clip_text(s,20+i*116,16,buttons[i],t->text,92);}
    s_clip_text(s,12,46,"Select an app. Built-ins cannot be uninstalled.",t->text,s->w-24);
    int rows=(s->h-110)/28,count=entries();if(rows<1)rows=1;if(d->scroll>count-rows)d->scroll=count>rows?count-rows:0;
    for(int r=0;r<rows;r++){struct app *a=entry(d->scroll+r);if(!a)break;int y=72+r*28;if(!strcmp(a->id,d->selected))s_fill(s,8,y,s->w-16,26,color_blend(t->win_bg,t->main,60));s_icon(s,a->icon,12,y,t->main);s_clip_text(s,44,y+5,a->title,t->text,s->w-240);s_clip_text(s,s->w-186,y+5,a->external?"CAT app":"Built-in",t->text,82);if(wm_taskbar_pinned(a->id))s_text(s,s->w-96,y+5,"Pinned",t->main);}
    char text[100],number[16];strcpy(text,"Bar limit: ");fmt_u32(number,wm_taskbar_capacity());strcat(text,number);strcat(text," | Uninstall keeps projects and data.");s_clip_text(s,12,s->h-24,text,t->text,s->w-24);
}
static void am_mouse(struct window *w,struct mouse_event *e,int x,int y){
    struct manager *d=w->data;if(e->type==MEV_WHEEL){d->scroll-=e->wheel*3;if(d->scroll<0)d->scroll=0;wm_redraw(w);return;}
    if(e->type!=MEV_BUTTON||!e->down||e->button!=MBTN_LEFT)return;
    if(y>=10&&y<38&&x>=12&&x<352){struct app *a=app_find(d->selected);if(!a)return;int b=(x-12)/116;if(b==0)wm_open_app(a->id,NULL);else if(b==1){if(!wm_taskbar_pin(a->id))wm_notify("Taskbar full","Unpin a launcher first. The limit adapts to screen width.",1);}else app_request_uninstall(a->id);wm_redraw(w);return;}
    if(y>=72&&y<w->surf.h-38&&x>=8&&x<w->surf.w-8){struct app *a=entry(d->scroll+(y-72)/28);if(a){strcpy(d->selected,a->id);wm_redraw(w);}}
}
struct app app_applications={.id="applications",.title="Applications",.desktop_label="Apps",.icon=ICON_SETTINGS,.single=1,.def_w=650,.def_h=520,.min_w=480,.min_h=300,.uses_data=1,.open=am_open,.paint=am_paint,.mouse=am_mouse,.close=am_close};
SCOS_APP(app_applications,013);
