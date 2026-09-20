/* Application registry, independent of compositor implementation and app list.
 * Built-ins self-register at link time; file-backed clients register at runtime. */
#include "scos.h"
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

struct window *app_open_document(const char *path)
{
    for(int i=0;i<reg_count;i++)if(registry[i]->file_editor)
        return wm_open_app(registry[i]->id,(void *)path);
    return NULL;
}
