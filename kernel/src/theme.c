/* SCos native - theme palettes (same four themes as the web simulation) */
#include "scos.h"

/* tiny json helpers */
static char *strstr_safe(const char *hay, const char *needle)
{
    int n = (int)strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!strncmp(p, needle, n)) return (char *)p;
    return NULL;
}

static char *strchr_q(char *p, char c, int skip)
{
    while (*p && skip > 0) {
        if (*p == c) skip--;
        p++;
    }
    return *p == c ? p : NULL;
}

static const struct theme themes[] = {
    { "matrix-1",       "Matrix",     0x39FF14, 0x031403, 0x000800, 0x1a1a1a, 0x39FF14, 0x001500, 0x141414 },
    { "blue-sky",       "Blue Sky",   0x00AAFF, 0x001933, 0x000a14, 0x002B4D, 0x00AAFF, 0x001020, 0x0d1b2a },
    { "midnight-purple","Purple",     0xCC66FF, 0x1A0033, 0x0a0014, 0x330066, 0xCC66FF, 0x140028, 0x1c1024 },
    { "amber-tech",     "Matrix red", 0xFF0000, 0x1a0000, 0x080000, 0x4D0000, 0xFF0000, 0x1a0000, 0x201010 },
};

static int current = 0;

int theme_count(void) { return (int)(sizeof(themes) / sizeof(themes[0])); }
const struct theme *theme_get(int i) { return &themes[i]; }
const struct theme *theme_current(void) { return &themes[current]; }
void theme_set_index(int i)
{
    if (i >= 0 && i < theme_count()) current = i;
}
int theme_index_of_id(const char *id)
{
    for (int i = 0; i < theme_count(); i++)
        if (!strcmp(themes[i].id, id)) return i;
    return -1;
}

void theme_load_from_settings(void)
{
    u32 len = 0;
    char *json = vfs_read("system/settings.json", &len);
    if (!json) return;
    char *p = strstr_safe(json, "\"theme\"");
    if (!p) return;
    p = strchr_q(p, '"', 2);
    if (!p) return;
    char id[32];
    int i = 0;
    while (p[i] && p[i] != '"' && i < 31) { id[i] = p[i]; i++; }
    id[i] = 0;
    int idx = theme_index_of_id(id);
    if (idx >= 0) theme_set_index(idx);
}

void settings_save(void)
{
    char buf[128];
    const struct theme *t = theme_current();
    /* manual format: no printf-to-buffer in the kernel */
    char *o = buf;
    strcpy(o, "{\n  \"theme\": \""); o += strlen(o);
    strcpy(o, t->id); o += strlen(o);
    strcpy(o, "\",\n  \"version\": \"2.0.0\"\n}"); o += strlen(o);
    vfs_write("system/settings.json", buf, (u32)(o - buf));
}

void system_reset(void)
{
    theme_set_index(0);
    vfs_init_defaults();          /* rebuilds the tree from scratch */
    settings_save();
}
