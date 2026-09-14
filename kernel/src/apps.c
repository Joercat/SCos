/* SCos native - application registry */
#include "scos.h"

extern struct app app_files, app_terminal, app_notepad, app_browser,
                  app_calendar, app_settings, app_about, app_blackjack, app_sysmon;

static struct app *registry[16];
static int reg_count;

void apps_register_all(void)
{
    reg_count = 0;
    registry[reg_count++] = &app_files;
    registry[reg_count++] = &app_terminal;
    registry[reg_count++] = &app_notepad;
    registry[reg_count++] = &app_browser;
    registry[reg_count++] = &app_calendar;
    registry[reg_count++] = &app_settings;
    registry[reg_count++] = &app_about;
    registry[reg_count++] = &app_blackjack;
    registry[reg_count++] = &app_sysmon;
    registry[reg_count++] = wm_dialog_app();
    registry[reg_count++] = wm_error_app();
}

struct app *app_find(const char *id)
{
    for (int i = 0; i < reg_count; i++)
        if (!strcmp(registry[i]->id, id)) return registry[i];
    return NULL;
}

int app_count(void) { return reg_count; }
struct app *app_at(int i) { return i < reg_count ? registry[i] : NULL; }
