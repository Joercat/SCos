/* Native C project editor/compiler/package workbench. Never a Lua app itself. */
#include "cat.h"
#define SOURCE_CAP (CAT_SOURCE_MAX+1)
#define CLIPBOARD_MAX 16384u
/* Session-local text only: bounded storage, no host clipboard or Lua access. */
static char clipboard[CLIPBOARD_MAX+1];
static u32 clipboard_len;
static const char starter[]="local count = 0\nreturn {\n    open = function() scos.title(\"My App\") end,\n    paint = function(w, h)\n        local t = scos.theme()\n        scos.clear(t.win_bg)\n        scos.text(20, 20, \"Count: \" .. count, t.text)\n        scos.button(20, 52, 180, 32, \"Click me\", false)\n    end,\n    mouse = function(kind, x, y, button, down)\n        if kind == 2 and button == 1 and down then\n            if scos.hit(x, y, 20, 52, 180, 32) then\n                count = count + 1\n                scos.redraw()\n            end\n        end\n    end\n}\n";
struct studio {char *source,*undo;u32 len,pos,undo_len,undo_pos;int undo_valid,dirty,focus,scroll,colscroll,selected,permission;char id[32],title[40],width[8],height[8],message[256];u32 revision,built_revision;int built,help;int field_pos[4],field_selected;char pending[192];u32 anchor;int selecting;};
static void message(struct window *w,const char *s){struct studio *d=w->data;strncpy(d->message,s,sizeof(d->message)-1);d->message[255]=0;wm_redraw(w);}
static void changed(struct window *w){struct studio *d=w->data;d->dirty=1;d->built=0;d->revision++;wm_redraw(w);}
static void checkpoint(struct studio *d){memcpy(d->undo,d->source,d->len+1);d->undo_len=d->len;d->undo_pos=d->pos;d->undo_valid=1;}
static u32 line_start(struct studio *d,u32 p){while(p&&d->source[p-1]!='\n')p--;return p;}
static u32 line_end(struct studio *d,u32 p){while(p<d->len&&d->source[p]!='\n')p++;return p;}
static u32 line_offset(struct studio *d,int line){u32 p=0;while(line--&&p<d->len){p=line_end(d,p);if(p<d->len)p++;}return p;}
static int rows(struct window *w){int n=(w->surf.h-174)/16;return n>0?n:1;}
static void reveal(struct window *w){struct studio *d=w->data;int row=0;for(u32 i=0;i<d->pos;i++)if(d->source[i]=='\n')row++;if(row<d->scroll)d->scroll=row;if(row>=d->scroll+rows(w))d->scroll=row-rows(w)+1;int col=d->pos-line_start(d,d->pos);int cols=(w->surf.w-254)/8;if(cols<1)cols=1;if(col<d->colscroll)d->colscroll=col;if(col>=d->colscroll+cols)d->colscroll=col-cols+1;}
static void selection(struct studio *d,u32 *a,u32 *b){*a=*b=d->pos;if(d->selected){*a=d->anchor<d->pos?d->anchor:d->pos;*b=d->anchor>d->pos?d->anchor:d->pos;}}
static void insert(struct window *w,const char *s,u32 n)
{
    struct studio *d=w->data;u32 a,b;selection(d,&a,&b);
    if(n>CAT_SOURCE_MAX-(d->len-(b-a))){message(w,"Source limit: 64 KiB. Paste/edit rejected; text unchanged.");return;}
    checkpoint(d);memmove(d->source+a+n,d->source+b,d->len-b+1);memcpy(d->source+a,s,n);
    d->len=d->len-(b-a)+n;d->pos=a+n;d->selected=0;changed(w);reveal(w);
}
static int number(const char *s){int v=0;if(!*s)return -1;while(*s){if(*s<'0'||*s>'9'||v>2048)return -1;v=v*10+*s++-'0';}return v;}
static int info(struct window *w,struct cat_info *m){struct studio *d=w->data;memset(m,0,sizeof(*m));strcpy(m->id,d->id);strcpy(m->title,d->title);m->width=number(d->width);m->height=number(d->height);m->permissions=d->permission?CAT_PERMISSION_THEME:0;m->source=d->source;m->length=d->len;for(int i=0;i<app_count();i++){struct app *a=app_at(i);if(!strcmp(a->id,d->id)&&!a->external){message(w,"That ID belongs to a built-in C app. Choose a different ID.");return 0;}}return 1;}
static void path_for(struct studio *d,char *p,int package){strcpy(p,package?"home/apps/":"home/projects/");strcat(p,d->id);strcat(p,package?".cat":".project");}
static void save(struct window *w){struct studio *d=w->data;struct cat_info m;char p[192],err[256];if(!info(w,&m))return;path_for(d,p,0);vfs_mkdir("home/projects");if(cat_project_save(p,&m,err,sizeof(err)))d->dirty=0;message(w,err);}
static int build(struct window *w){struct studio *d=w->data;struct cat_info m;char p[192],err[256];d->built=0;if(!info(w,&m))return 0;path_for(d,p,1);vfs_mkdir("home/apps");if(!cat_build(p,&m,err,sizeof(err))){message(w,err);return 0;}d->built=1;d->built_revision=d->revision;message(w,err);return 1;}
static void fresh(struct window *w){struct studio *d=w->data;strcpy(d->source,starter);d->len=strlen(starter);d->pos=0;strcpy(d->id,"my-app");strcpy(d->title,"My App");strcpy(d->width,"560");strcpy(d->height,"360");d->scroll=d->colscroll=d->selected=d->permission=d->undo_valid=d->built=0;d->focus=4;memset(d->field_pos,0,sizeof(d->field_pos));changed(w);message(w,"New project. Ctrl+S saves .project; Ctrl+B builds .cat; F5 builds and runs.");}
static struct window *live(void *ud){for(int i=0;i<wm_win_count();i++){struct window *w=wm_win_at(i);if(w->id==(int)(uintptr_t)ud&&w->app&&!strcmp(w->app->id,"studio"))return w;}return NULL;}
static void new_answer(int ok,const char *text,void *ud){(void)text;struct window *w=live(ud);if(w&&ok)fresh(w);}
static void load_project(struct window *w,const char *path){struct studio *d=w->data;u32 n=0;const char *s=vfs_read(path,&n);struct cat_info m;char err[256];if(!s){message(w,"Project not found");return;}int ok=n>=8&&!memcmp(s,"SCOSCAT1",8)?cat_validate(s,n,&m,err,sizeof(err)):cat_project_load(s,n,&m,err,sizeof(err));if(!ok){message(w,err);return;}memcpy(d->source,m.source,m.length);d->source[m.length]=0;d->len=m.length;d->pos=0;strcpy(d->id,m.id);strcpy(d->title,m.title);fmt_u32(d->width,m.width);fmt_u32(d->height,m.height);d->permission=!!(m.permissions&CAT_PERMISSION_THEME);d->scroll=d->colscroll=d->selected=d->undo_valid=d->built=d->dirty=0;d->revision++;d->focus=4;memset(d->field_pos,0,sizeof(d->field_pos));message(w,"Project opened. Build creates a checked .cat; Run never uses unsaved raw code.");}
static void open_answer(int ok,const char *text,void *ud){struct window *w=live(ud);if(w&&ok&&text)load_project(w,text);}
static void open_dialog(struct window *w){wm_dialog("Open project / CAT","Path to .project or .cat (replaces current editor):","home/projects/my-app.project",open_answer,(void *)(uintptr_t)w->id);}
static void discard_open(int ok,const char *text,void *ud){(void)text;struct window *w=live(ud);if(w&&ok)open_dialog(w);}
static void recover_answer(int ok,const char *text,void *ud){(void)text;struct window *w=live(ud);if(w&&ok)load_project(w,"home/projects/studio-recovery.project");}
static void recovery(struct window *w){struct studio *d=w->data;if(!d->dirty)return;struct cat_info m;memset(&m,0,sizeof(m));strcpy(m.id,d->id);strcpy(m.title,d->title);m.width=number(d->width);m.height=number(d->height);m.source=d->source;m.length=d->len;m.permissions=d->permission?1:0;char err[256];vfs_mkdir("home/projects");if(!cat_project_save("home/projects/studio-recovery.project",&m,err,sizeof(err))){strcpy(m.id,"recovery");strcpy(m.title,"Recovered project");m.width=560;m.height=360;if(!cat_project_save("home/projects/studio-recovery.project",&m,err,sizeof(err)))klog("App Studio recovery failed: %s",err);}}
static void action(struct window *w,int a){struct studio *d=w->data;if(wm_dialog_active())return;if(a==0){if(d->dirty)wm_dialog("New project","Discard unsaved editor changes?",NULL,new_answer,(void *)(uintptr_t)w->id);else fresh(w);}else if(a==1){if(d->dirty)wm_dialog("Open project","Discard unsaved editor changes?",NULL,discard_open,(void *)(uintptr_t)w->id);else open_dialog(w);}else if(a==2)save(w);else if(a==3){char err[256];lua_source_check(d->source,d->len,err,sizeof(err));message(w,err);}else if(a==4)build(w);else if(a==5){if(build(w)){char p[192];path_for(d,p,1);struct app *ap=lua_app_install(p);struct window *child=ap?wm_open_app(ap->id,NULL):NULL;if(!child)message(w,"Cannot launch: ID/path conflict, registry full, or window limit");else if(child->app->failure&&child->app->failure(child))message(w,child->app->failure(child));else message(w,"Built package running in a separate window; close it before testing edits again.");}}else if(a==6){d->help=!d->help;wm_redraw(w);}else if(a==7){if(d->dirty)wm_dialog("Recover project","Replace editor with the RAM recovery copy?",NULL,recover_answer,(void *)(uintptr_t)w->id);else load_project(w,"home/projects/studio-recovery.project");}}
static void document_answer(int ok,const char *text,void *ud){(void)text;struct window *w=live(ud);if(w&&ok)load_project(w,((struct studio *)w->data)->pending);}
static void st_document(struct window *w,const char *path){struct studio *d=w->data;if(strlen(path)>=sizeof(d->pending)){message(w,"Project path too long");return;}if(wm_dialog_active()){message(w,"Finish the open dialog first");return;}strcpy(d->pending,path);if(d->dirty)wm_dialog("Open project","Discard unsaved editor changes?",NULL,document_answer,(void *)(uintptr_t)w->id);else load_project(w,path);}
static void st_open(struct window *w,void *arg){struct studio *d=palloc(sizeof(*d));if(!d)return;memset(d,0,sizeof(*d));d->source=palloc(SOURCE_CAP);d->undo=palloc(SOURCE_CAP);if(!d->source||!d->undo){if(d->source)pfree(d->source,SOURCE_CAP);if(d->undo)pfree(d->undo,SOURCE_CAP);pfree(d,sizeof(*d));return;}w->data=d;fresh(w);d->dirty=0;if(arg)load_project(w,arg);}
static void st_close(struct window *w){struct studio *d=w->data;if(d){recovery(w);pfree(d->source,SOURCE_CAP);pfree(d->undo,SOURCE_CAP);pfree(d,sizeof(*d));}w->data=NULL;}
static void drawline(struct surface *s,int x,int y,const char *p,int n,const struct theme *t){char b[256];if(n>250)n=250;memcpy(b,p,n);b[n]=0;u32 c=t->text;if(n>=2&&p[0]=='-'&&p[1]=='-')c=color_blend(t->text,t->win_bg,45);s_text(s,x,y,b,c);}
/* Lightweight lexical coloring; the Lua compiler, not these hints, validates code. */
static void draw_code(struct surface *s,int x,int y,const char *p,int n,const struct theme *t)
{
    drawline(s,x,y,p,n,t);
    static const char *keywords[]={"local","function","return","end","if","then","else","elseif","for","while","do","repeat","until","break","true","false","nil","and","or","not","in"};
    for(int i=0;i<n;){int start=i;u32 c=t->text;
        if(p[i]=='-'&&i+1<n&&p[i+1]=='-'){c=color_blend(t->text,t->win_bg,40);i=n;}
        else if(p[i]=='"'||p[i]=='\''){char quote=p[i++];while(i<n){if(p[i]=='\\'&&i+1<n){i+=2;continue;}if(p[i++]==quote)break;}c=color_blend(t->main,t->text,50);}
        else if((p[i]>='a'&&p[i]<='z')||(p[i]>='A'&&p[i]<='Z')||p[i]=='_'){while(i<n&&((p[i]>='a'&&p[i]<='z')||(p[i]>='A'&&p[i]<='Z')||(p[i]>='0'&&p[i]<='9')||p[i]=='_'))i++;for(unsigned k=0;k<sizeof(keywords)/sizeof(keywords[0]);k++)if(strlen(keywords[k])==(size_t)(i-start)&&!strncmp(p+start,keywords[k],i-start)){c=t->main;break;}}
        else if(p[i]>='0'&&p[i]<='9'){while(i<n&&((p[i]>='0'&&p[i]<='9')||p[i]=='.'))i++;c=color_blend(t->main,t->text,40);}
        else{i++;continue;}
        char token[256];int len=i-start;if(len>250)len=250;memcpy(token,p+start,len);token[len]=0;s_text(s,x+start*8,y,token,c);
    }
}
static void st_paint(struct window *w){struct studio *d=w->data;struct surface *s=&w->surf;const struct theme *t=theme_current();s_fill(s,0,0,s->w,s->h,t->win_bg);int bw=(s->w-16)/8;const char *buttons[]={"New","Open","Save","Check","Build","Run","API","Recover"};for(int i=0;i<8;i++){s_frame_rect(s,8+i*bw,8,bw-4,28,t->main);s_text(s,16+i*bw,16,buttons[i],t->text);}s_fill(s,8,46,180,s->h-54,color_blend(t->win_bg,t->main,8));s_text(s,16,56,d->dirty?"PROJECT *":"PROJECT",t->main);const char *labels[]={"App ID","Title","Width","Height"};const char *values[]={d->id,d->title,d->width,d->height};for(int i=0;i<4;i++){int y=84+i*44;s_text(s,16,y,labels[i],t->text);s_frame_rect(s,16,y+16,164,22,d->focus==i?t->main:t->text);int start=d->focus==i&&d->field_pos[i]>18?d->field_pos[i]-18:0;if(d->focus==i&&d->field_selected)s_fill(s,18,y+18,158,18,color_blend(t->win_bg,t->main,35));s_clip_text(s,20,y+21,values[i]+start,t->text,154);if(d->focus==i&&(tick_count/50)%2==0)s_fill(s,20+(d->field_pos[i]-start)*8,y+20,2,16,t->main);}s_text(s,16,274,d->permission?"[x] Theme access":"[ ] Theme access",t->main);s_text(s,16,300,"Output: .cat",t->text);s_clip_text(s,16,320,d->id,t->text,158);s_text(s,16,346,d->built?"Build: current":"Build: needed",t->main);s_text(s,16,374,"Ctrl+S save",t->text);s_text(s,16,394,"Ctrl+B build",t->text);s_text(s,16,414,"F5 build + run",t->text);s_text(s,16,434,"Ctrl+Z/Y undo/redo",t->text);s_text(s,16,454,"Ctrl+C/X/V copy",t->text);s_text(s,16,474,"Clipboard: 16 KiB",t->text);s_text(s,16,s->h-38,"RAM files only*",t->main);
    int ex=198,ey=54;int erows=rows(w),cols=(s->w-254)/8;
    s_text(s,ex,42,d->help?"API QUICK REFERENCE (API closes)":"main.lua - source editor",t->main);
    if(d->help){const char *help[]={"return { open=..., paint=..., mouse=..., key=..., tick=... }","paint(w,h): clear / rect / frame / line / circle / disc","text / text_width / text_scaled / gradient / pixel / icon","button / checkbox / progress / hit / rgb / blend / clamp","theme() / themes() / theme_apply(id) / theme_custom(id,table)","Theme fields: main,bg_top,bg_bot,win_bg,text,title_text,taskbar_bg","Background: mode (0 solid,1 gradient,2 grid,3 stripes),spacing,grid","read / write / exists / remove / files / file_size / rename","size / window / screen / title / redraw / mouse / interval","resize / move / message / log / theme_status",
"time / date / uptime / memory / app_id / version / api_info","Theme requests need the checkbox + interactive user approval.","Projects preserve source and metadata. Packages verify CRC + syntax.","Sources are editable; only .cat files are launched.","Built-in system applications are compiled C, never Lua files.","USB boots cannot persist these files across reboot.","Use the confirmed Terminal save command on supported ATA disks."};for(unsigned i=0;i<sizeof(help)/sizeof(help[0])&&(int)i<erows;i++)s_clip_text(s,ex,ey+i*18,help[i],t->text,s->w-ex-8);}else{u32 p=line_offset(d,d->scroll);for(int row=0;row<erows&&p<=d->len;row++){u32 end=line_end(d,p);char num[16];fmt_u32(num,d->scroll+row+1);s_text(s,ex,ey+row*16,num,t->main);u32 start=p+(u32)d->colscroll;if(start>end)start=end;int n=end-start;if(n>cols)n=cols;if(d->selected){u32 a,b;selection(d,&a,&b);if(a<start)a=start;if(b>start+(u32)n)b=start+n;if(b>a)s_fill(s,ex+48+(a-start)*8,ey+row*16,(b-a)*8,16,color_blend(t->win_bg,t->main,25));}draw_code(s,ex+48,ey+row*16,d->source+start,n,t);if(d->focus==4&&d->pos>=start&&d->pos<=end&&(tick_count/50)%2==0){int cx=ex+48+(d->pos-start)*8;if(cx<s->w-8)s_fill(s,cx,ey+row*16,2,12,t->main);}if(end==d->len)break;p=end+1;}}
    int y=s->h-100;s_frame_rect(s,198,y,s->w-206,92,t->main);s_text(s,206,y+6,"BUILD / DIAGNOSTICS",t->main);int width=(s->w-222)/8;if(width<1)width=1;size_t off=0,len=strlen(d->message);for(int r=0;r<3&&off<len;r++){int n=len-off;if(n>width)n=width;drawline(s,206,y+25+r*16,d->message+off,n,t);off+=n;}char status[120],num[24];u32 line=1;for(u32 i=0;i<d->pos;i++)if(d->source[i]=='\n')line++;strcpy(status,"Ln ");fmt_u32(num,line);strcat(status,num);strcat(status," Col ");fmt_u32(num,d->pos-line_start(d,d->pos)+1);strcat(status,num);strcat(status," | ");fmt_u32(num,d->len);strcat(status,num);strcat(status,"/65536 bytes | Clip ");fmt_u32(num,clipboard_len);strcat(status,num);strcat(status,"/16384 | RAM");s_clip_text(s,206,s->h-20,status,t->text,s->w-214);
}
static void clipboard_action(struct window *w,int key)
{
    struct studio *d=w->data;
    if(d->focus<4){
        char *p=d->focus==0?d->id:d->focus==1?d->title:d->focus==2?d->width:d->height;
        u32 len=strlen(p),max=d->focus==0?30:d->focus==1?39:7;
        if(key!='v'){
            if(!d->field_selected){message(w,"Select field text with Ctrl+A first");return;}
            memcpy(clipboard,p,len);clipboard[len]=0;clipboard_len=len;
            if(key=='x'){p[0]=0;d->field_pos[d->focus]=0;d->field_selected=0;changed(w);}
        }else{
            u32 pos=d->field_selected?0:(u32)d->field_pos[d->focus];
            u32 keep=d->field_selected?0:len;
            if(!clipboard_len){message(w,"Clipboard empty");return;}
            if(clipboard_len>max-keep){message(w,"Paste exceeds field limit; field unchanged");return;}
            for(u32 i=0;i<clipboard_len;i++)if((u8)clipboard[i]<32||(u8)clipboard[i]>126){message(w,"Field paste requires printable ASCII");return;}
            if(d->field_selected)p[0]=0;
            memmove(p+pos+clipboard_len,p+pos,keep-pos+1);memcpy(p+pos,clipboard,clipboard_len);
            d->field_pos[d->focus]=pos+clipboard_len;d->field_selected=0;changed(w);
        }
        wm_redraw(w);return;
    }
    u32 a,b;selection(d,&a,&b);
    if(key=='v'){
        if(!clipboard_len){message(w,"Clipboard empty");return;}
        insert(w,clipboard,clipboard_len);return;
    }
    if(a==b){message(w,"Select text: Shift+arrows, mouse drag, or Ctrl+A");return;}
    if(b-a>CLIPBOARD_MAX){message(w,"Copy limit: 16 KiB. Text and clipboard unchanged.");return;}
    memcpy(clipboard,d->source+a,b-a);clipboard_len=b-a;clipboard[clipboard_len]=0;
    if(key=='x')insert(w,"",0);
    message(w,key=='x'?"Selection cut (Ctrl+Z undoes)":"Selection copied (maximum 16 KiB)");
}
static void st_key(struct window *w,struct key_event *e)
{
    struct studio *d=w->data;if(!e->pressed)return;int k=e->keycode;
    if(e->ctrl){
        if(k=='s'||k==19){action(w,2);return;}if(k=='b'||k==2){action(w,4);return;}
        if(k=='o'||k==15){action(w,1);return;}if(k=='n'||k==14){action(w,0);return;}
    }
    if(k==KEY_F5){action(w,5);return;}if(k==KEY_F1){action(w,6);return;}if(d->help)return;
    if(e->ctrl&&(k=='c'||k==3||k=='x'||k==24||k=='v'||k==22)){
        clipboard_action(w,k=='c'||k==3?'c':k=='x'||k==24?'x':'v');return;
    }
    if(d->focus<4){
        char *p=d->focus==0?d->id:d->focus==1?d->title:d->focus==2?d->width:d->height;
        int max=d->focus==0?30:d->focus==1?39:7;
        if(e->ctrl&&(k=='a'||k==1)){d->field_selected=1;wm_redraw(w);return;}if(e->ctrl)return;
        if(d->field_selected&&(k=='\b'||k==KEY_DELETE||(k>=32&&k<127))){p[0]=0;d->field_pos[d->focus]=0;d->field_selected=0;changed(w);if(k==KEY_DELETE||k=='\b')return;}
        else d->field_selected=0;
        if(edit_line(p,&d->field_pos[d->focus],max+1,e))changed(w);else wm_redraw(w);return;
    }
    if(e->ctrl&&(k=='a'||k==1)){d->anchor=0;d->pos=d->len;d->selected=d->len!=0;wm_redraw(w);return;}
    if(e->ctrl&&(k=='z'||k==26||k=='y'||k==25)){
        if(d->undo_valid){char *tmp=d->source;d->source=d->undo;d->undo=tmp;u32 n=d->len,p=d->pos;d->len=d->undo_len;d->pos=d->undo_pos;d->undo_len=n;d->undo_pos=p;d->selected=0;changed(w);reveal(w);}return;
    }
    if(k==KEY_LEFT||k==KEY_RIGHT||k==KEY_HOME||k==KEY_END||k==KEY_UP||k==KEY_DOWN||k==KEY_PGUP||k==KEY_PGDN){
        if(e->shift&&!d->selected)d->anchor=d->pos;
        if(k==KEY_LEFT){if(d->pos)d->pos--;}
        else if(k==KEY_RIGHT){if(d->pos<d->len)d->pos++;}
        else if(k==KEY_HOME)d->pos=e->ctrl?0:line_start(d,d->pos);
        else if(k==KEY_END)d->pos=e->ctrl?d->len:line_end(d,d->pos);
        else{int steps=(k==KEY_PGUP||k==KEY_PGDN)?rows(w):1;while(steps--){u32 start=line_start(d,d->pos),col=d->pos-start;if(k==KEY_UP||k==KEY_PGUP){if(!start)break;d->pos=line_start(d,start-1);}else{d->pos=line_end(d,d->pos);if(d->pos<d->len)d->pos++;}u32 end=line_end(d,d->pos);d->pos=d->pos+col>end?end:d->pos+col;}}
        d->selected=e->shift&&d->anchor!=d->pos;
    }else if(e->ctrl)return;
    else if(k=='\b'||k==KEY_DELETE){
        if(!d->selected){d->anchor=d->pos;if(k=='\b'&&d->pos)d->anchor--;else if(k==KEY_DELETE&&d->pos<d->len)d->anchor++;d->selected=d->anchor!=d->pos;}
        if(d->selected)insert(w,"",0);
    }else if(k=='\n'){
        char text[65];text[0]='\n';u32 start=line_start(d,d->pos),n=0;
        if(!d->selected)while(start+n<d->pos&&d->source[start+n]==' '&&n<64){text[n+1]=' ';n++;}
        insert(w,text,n+1);
    }else if(k=='\t')insert(w,"    ",4);
    else if(k>=32&&k<127){char c=k;insert(w,&c,1);}
    reveal(w);wm_redraw(w);
}
static void st_mouse(struct window *w,struct mouse_event *e,int x,int y){struct studio *d=w->data;if(e->type==MEV_WHEEL){d->scroll-=e->wheel*3;if(d->scroll<0)d->scroll=0;int total=1;for(u32 i=0;i<d->len;i++)if(d->source[i]=='\n')total++;if(d->scroll>total-1)d->scroll=total-1;wm_redraw(w);return;}if(e->type==MEV_BUTTON&&!e->down&&e->button==MBTN_LEFT){d->selecting=0;return;}
if(e->type==MEV_MOVE&&d->selecting){if(!(e->buttons&MBTN_LEFT)){d->selecting=0;return;}int row=(y-54)/16;if(row<0)row=0;if(row>=rows(w))row=rows(w)-1;int col=x<246?0:(x-246)/8;u32 start=line_offset(d,d->scroll+row),end=line_end(d,start),pos=start+d->colscroll+col;d->pos=pos>end?end:pos;d->selected=d->pos!=d->anchor;wm_redraw(w);return;}
if(e->type!=MEV_BUTTON||!e->down||e->button!=MBTN_LEFT)return;
if(y>=8&&y<36&&x>=8){int a=(x-8)/((w->surf.w-16)/8);if(a<8)action(w,a);return;}if(x>=16&&x<180){for(int i=0;i<4;i++)if(y>=100+i*44&&y<122+i*44){d->focus=i;d->field_selected=0;d->field_pos[i]=strlen(i==0?d->id:i==1?d->title:i==2?d->width:d->height);wm_redraw(w);return;}if(y>=270&&y<292){d->permission=!d->permission;changed(w);return;}}if(!d->help&&x>=246&&y>=54&&y<54+rows(w)*16){d->focus=4;d->selected=0;u32 start=line_offset(d,d->scroll+(y-54)/16),end=line_end(d,start);u32 pos=start+d->colscroll+(x-246)/8;d->pos=pos>end?end:pos;d->anchor=d->pos;d->selecting=1;wm_redraw(w);}}
static void st_tick(struct window *w){if(tick_count%50==0)wm_redraw(w);}
struct app app_studio={.id="studio",.title="App Studio",.desktop_label="App Studio",.icon=ICON_TERMINAL,.single=1,.def_w=980,.def_h=690,.min_w=740,.min_h=560,.uses_data=1,.file_suffix=".project",.document=st_document,.open=st_open,.paint=st_paint,.key=st_key,.mouse=st_mouse,.tick=st_tick,.close=st_close};
SCOS_APP(app_studio,012);
