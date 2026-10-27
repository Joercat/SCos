/* scoswm-arch desktop preferences, ported back to native SCos.
 *
 * The Linux WM kept its live desktop settings in ~/.config/scoswm/scoswm.conf:
 * bar_height, desktop_icon_size, desktop_grid, plus custom palette fields.
 * SCos already owns its themes, so this file ports the three layout switches
 * and adds the WM behaviour switches the Linux version hard-coded on.  Every
 * one is an owner choice, persisted in the VFS (and therefore in the vault)
 * as home/desktop.cfg.  Defaults reproduce the original SCos desktop exactly.
 */
#include "scos.h"

#define CFG_PATH "home/desktop.cfg"

struct wm_prefs {
    int bar_height, icon_size, grid, hotkeys, overview, snap;
};
static struct wm_prefs prefs = { 40, 24, 1, 1, 1, 1 };

static const char *const names[WMP_COUNT] = {
    "bar_height", "desktop_icon_size", "desktop_grid",
    "hotkeys", "overview", "snap"
};

int wm_taskbar_height(void) { return prefs.bar_height; }

const char *wm_desktop_pref_name(int key)
{
    return key >= 0 && key < WMP_COUNT ? names[key] : "";
}

int wm_desktop_pref(int key)
{
    switch (key) {
        case WMP_BAR_HEIGHT: return prefs.bar_height;
        case WMP_ICON_SIZE:  return prefs.icon_size;
        case WMP_GRID:       return prefs.grid;
        case WMP_HOTKEYS:    return prefs.hotkeys;
        case WMP_OVERVIEW:   return prefs.overview;
        case WMP_SNAP:       return prefs.snap;
    }
    return 0;
}

/* Clamp rather than reject, so a hand-edited config file cannot produce a
 * taskbar taller than the screen or an icon size the painter cannot draw. */
static int clamp_pref(int key, int v)
{
    switch (key) {
        case WMP_BAR_HEIGHT:
            if (v < 28) v = 28;
            if (v > 96) v = 96;
            if (v > screen_h / 3) v = screen_h / 3;
            return v & ~3;
        case WMP_ICON_SIZE:
            return v <= 24 ? 24 : v <= 32 ? 32 : 48;
        default:
            return v ? 1 : 0;
    }
}

int wm_desktop_pref_set(int key, int value)
{
    if (key < 0 || key >= WMP_COUNT) return 0;
    value = clamp_pref(key, value);
    switch (key) {
        case WMP_BAR_HEIGHT: prefs.bar_height = value; break;
        case WMP_ICON_SIZE:  prefs.icon_size  = value; break;
        case WMP_GRID:       prefs.grid       = value; break;
        case WMP_HOTKEYS:    prefs.hotkeys    = value; break;
        case WMP_OVERVIEW:   prefs.overview   = value; break;
        case WMP_SNAP:       prefs.snap       = value; break;
    }
    wm_prefs_save();
    /* A layout change invalidates the cached wallpaper and the whole scene. */
    wm_wallpaper_invalidate();
    if (key == WMP_BAR_HEIGHT) wm_reflow_windows();
    wm_request_full();
    return wm_desktop_pref(key);
}

void wm_prefs_save(void)
{
    char buf[320], num[16];
    strcpy(buf, "# SCos desktop preferences (from Settings)\n");
    for (int i = 0; i < WMP_COUNT; i++) {
        strcat(buf, names[i]);
        strcat(buf, " = ");
        fmt_u32(num, (u32)wm_desktop_pref(i));
        strcat(buf, num);
        strcat(buf, "\n");
    }
    /* Kernel-owned metadata: an app's mount namespace must not be able to
     * prevent the desktop from saving its own settings. */
    security_bypass_scope(1);
    vfs_write(CFG_PATH, buf, (u32)strlen(buf));
    security_bypass_scope(0);
}

static int parse_u32(const char *s, u32 *out)
{
    u32 v = 0;
    if (!s || !*s) return 0;
    while (*s >= '0' && *s <= '9') {
        if (v > 1000000) return 0;
        v = v * 10 + (u32)(*s++ - '0');
    }
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (*s) return 0;
    *out = v;
    return 1;
}

void wm_prefs_init(void)
{
    u32 n = 0;
    char *data = vfs_read(CFG_PATH, &n);
    if (!data || n > 4096) return;
    for (char *line = data; *line && (size_t)(line - data) < n;) {
        char *end = line;
        while (*end && *end != '\n' && *end != '\r') end++;
        char saved = *end;
        *end = 0;
        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = 0;
            char *key = line, *val = eq + 1;
            while (*key == ' ' || *key == '\t') key++;
            while (*val == ' ' || *val == '\t') val++;
            char *kend = key + strlen(key);
            while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = 0;
            u32 v;
            if (key[0] != '#' && parse_u32(val, &v))
                for (int i = 0; i < WMP_COUNT; i++)
                    if (!strcmp(key, names[i])) {
                        int clamped = clamp_pref(i, (int)v);
                        switch (i) {
                            case WMP_BAR_HEIGHT: prefs.bar_height = clamped; break;
                            case WMP_ICON_SIZE:  prefs.icon_size  = clamped; break;
                            case WMP_GRID:       prefs.grid       = clamped; break;
                            case WMP_HOTKEYS:    prefs.hotkeys    = clamped; break;
                            case WMP_OVERVIEW:   prefs.overview   = clamped; break;
                            case WMP_SNAP:       prefs.snap       = clamped; break;
                        }
                    }
            *eq = '=';
        }
        *end = saved;
        line = *end ? end + 1 : end;
    }
}
