/* Application registry, independent of compositor implementation and app list.
 * Built-ins self-register at link time; file-backed clients register at runtime. */
#include "cat.h"
#define APP_LIMIT 64
static struct app *registry[APP_LIMIT];
static int reg_count;
extern struct app * const __scos_apps_start[], * const __scos_apps_end[];
int app_register(struct app *a)
{
    if(!a||!a->id||!a->title||!a->id[0]||strlen(a->id)>31||strlen(a->title)>39||(a->desktop_label&&strlen(a->desktop_label)>39)||reg_count>=APP_LIMIT)return 0;
    for(int i=0;i<reg_count;i++)if(!strcmp(registry[i]->id,a->id))return registry[i]==a;
    registry[reg_count++]=a;return 1;
}
void apps_register_all(void)
{
    reg_count=0;
    for(struct app * const *p=__scos_apps_start;p<__scos_apps_end;p++)
        if(!app_register(*p))panic("invalid native app registration");
    lua_apps_refresh();
}
struct app *app_find(const char *id)
{
    if(!id)return NULL;
    for(int i=0;i<reg_count;i++)if(!strcmp(registry[i]->id,id))return registry[i];
    return lua_app_install(id);
}
int app_count(void){return reg_count;}
struct app *app_at(int i){return i>=0&&i<reg_count?registry[i]:NULL;}

/* One native modal at a time; copy the path rather than retaining a caller pointer. */
static char install_path[192];
static void install_answer(int ok,const char *text,void *ud)
{
    (void)text;(void)ud;if(!ok)return;
    u32 n=0;char *bytes=vfs_read(install_path,&n),err[256];struct cat_info m;
    if(!cat_validate(bytes,n,&m,err,sizeof(err))||!lua_source_check(m.source,m.length,err,sizeof(err))){wm_error_popup(err);return;}
    char dest[96];strcpy(dest,"home/apps/");strcat(dest,m.id);strcat(dest,".cat");
    for(int i=0;i<reg_count;i++)if(!strcmp(registry[i]->id,m.id)){wm_error_popup("Application ID already registered.\nNo files were replaced.");return;}
    if(vfs_lookup(dest)){wm_error_popup("Package already exists in home/apps.\nOpen that copy; no files replaced.");return;}
    if(!vfs_lookup("home/apps"))vfs_mkdir("home/apps");
    if(!vfs_write(dest,bytes,n)){wm_error_popup("Package install failed: cannot write RAM file.");return;}
    struct app *a=lua_app_install(dest);
    if(!a){vfs_delete(dest);wm_error_popup("Package registration failed. Install rolled back.");return;}
    wm_open_app(a->id,NULL);
}

struct window *app_open_document(const char *path)
{
    size_t n=strlen(path);
    if(n>=4&&!strcmp(path+n-4,".cat")){
        const char *relative=path[0]=='/'?path+1:path;
        if(strncmp(relative,"home/apps/",10)){
            if(wm_dialog_active())return NULL;
            if(strlen(path)>=sizeof(install_path)){wm_error_popup("Package path too long");return NULL;}
            u32 size=0;char *bytes=vfs_read(path,&size),err[256];struct cat_info m;
            if(!cat_validate(bytes,size,&m,err,sizeof(err))){wm_error_popup(err);return NULL;}
            strcpy(install_path,path);
            wm_dialog("Install CAT application","Install this package into home/apps and run?\nOnly install code you trust.\nSaved in RAM; disk persistence requires save.",NULL,install_answer,NULL);
            return NULL;
        }
        struct app *a=lua_app_install(path);
        if(a)return wm_open_app(a->id,NULL);
        wm_error_popup("Cannot open CAT package.\nInvalid format, conflicting ID,\nor application registry full.");return NULL;
    }
    for(int i=0;i<reg_count;i++)if(registry[i]->file_suffix){
        size_t len=strlen(registry[i]->file_suffix);
        if(n>=len&&!strcmp(path+n-len,registry[i]->file_suffix))return wm_open_app(registry[i]->id,(void *)path);
    }
    for(int i=0;i<reg_count;i++)if(registry[i]->file_editor)
        return wm_open_app(registry[i]->id,(void *)path);
    return NULL;
}
