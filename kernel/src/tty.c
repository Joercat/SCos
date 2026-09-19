/* SCos native - kernel console ("tty").
 *
 * r36 introduced this as a rescue shell.  r37 makes it what the field
 * feedback asked for: the LINUX MODEL - a kernel-owned text console that
 * runs the WHOLE OS, with the window manager merely layered on top.
 * Every command the terminal app offers works here too (filesystem,
 * calc, sysinfo, themes, calendar, sysrq, diagnostics, process control);
 * the ONLY commands that need the WM are the ones that launch or kill
 * GUI apps, and those fail with an explicit
 *     "app failed startup: cannot bind to wm"
 * style error instead of silently doing nothing.
 *
 * Entry points:
 *   - the terminal command `tty` (raises tty_request; the WM main loop
 *     hands screen + keyboard over and reclaims them when tty returns),
 *   - Ctrl+Alt+F1 anywhere in the WM (r37 hotkey, like Linux's console
 *     switch),
 *   - wm_run() ever RETURNING (the WM died - kmain falls through here),
 * and while this console runs it ALSO presents pending non-fatal error
 * screens (err_show_pending) - error display no longer depends on the
 * WM being alive.  Kernel panics never did (panic.c draws straight to
 * the framebuffer).
 *
 * Like everything else in SCos this is polled: each loop pumps
 * usb_poll() (the keyboard is a USB device - without this the console
 * itself would be deaf), drains kbd_poll(), edits the line with the
 * shared edit_line() helper and repaints the 8x16 text grid at the
 * FONT_H+2 pitch term_dump.py decodes.
 */
#include "scos.h"

int tty_request;                     /* raised by the terminal `tty` cmd
                                      * or the WM's Ctrl+Alt+F1 hotkey */

#define TTY_COLS      96
#define TTY_ROWS_MAX  64
#define TTY_BG        0x000A0A12u
#define TTY_FG        0x00C8C8C8u
#define TTY_PROMPT_FG 0x00E8E8A0u
#define TTY_WARN_FG   0x00E8A060u

static char tty_lines[TTY_ROWS_MAX][TTY_COLS + 1];
static int  tty_nlines;
static char tty_input[TTY_COLS + 1];
static int  tty_ipos;
static int  tty_rows;
static int  tty_wm_alive;
static int  tty_exit;
static int  wm_restarts;             /* 'wm' restart budget after a death */
static char tty_cwd[128] = "/";      /* r37: the console has its own cwd */

static void tty_line_out(const char *s, int len)
{
    if (tty_nlines >= TTY_ROWS_MAX) {
        /* scroll the history up: oldest line is dropped */
        for (int i = 1; i < TTY_ROWS_MAX; i++)
            memcpy(tty_lines[i - 1], tty_lines[i], TTY_COLS + 1);
        tty_nlines = TTY_ROWS_MAX - 1;
    }
    if (len > TTY_COLS) len = TTY_COLS;
    memcpy(tty_lines[tty_nlines], s, (u32)len);
    tty_lines[tty_nlines][len] = 0;
    tty_nlines++;
}

static void tty_print(const char *s)
{
    while (s && *s) {
        char *nl = str_chr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        /* wrap long lines (cat output) instead of truncating them */
        while (len > TTY_COLS) {
            tty_line_out(s, TTY_COLS);
            s += TTY_COLS;
            len -= TTY_COLS;
        }
        tty_line_out(s, len);
        if (!nl) break;
        s = nl + 1;
    }
}

/* Linux-style prompt: scos:<cwd># */
static void tty_prompt(char *out, int max)
{
    strcpy(out, "scos:");
    int used = 5;
    int cl = (int)strlen(tty_cwd);
    if (cl > max - used - 3) cl = max - used - 3;   /* clamp long cwds */
    if (cl > 0) { memcpy(out + used, tty_cwd, (u32)cl); used += cl; }
    out[used++] = '#';
    out[used++] = ' ';
    out[used] = 0;
}

static void tty_draw(void)
{
    u32 *p = screen.px;
    u32 n = (u32)screen.w * (u32)screen.h;
    for (u32 i = 0; i < n; i++) p[i] = TTY_BG;
    int y = 4;
    int first = tty_nlines - (tty_rows - 1);
    if (first < 0) first = 0;
    for (int i = first; i < tty_nlines; i++, y += FONT_H + 2)
        s_text(&screen, 6, y, tty_lines[i], TTY_FG);
    char pl[TTY_COLS + 4];
    tty_prompt(pl, TTY_COLS - 2);
    strncat(pl, tty_input, TTY_COLS - (int)strlen(pl));
    s_text(&screen, 6, y, pl, TTY_PROMPT_FG);
    /* block cursor after the input position */
    int plen = (int)strlen(pl) - (int)strlen(tty_input);
    s_fill(&screen, 6 + (plen + tty_ipos) * FONT_W, y + FONT_H - 2,
           FONT_W, 2, TTY_FG);
    fb_flip();
}

/* resolve a possibly-relative path against the console cwd */
static void tty_resolve(const char *arg, char *out, int max)
{
    if (!arg || !arg[0] || arg[0] == '/') {
        strncpy(out, (arg && arg[0]) ? arg : "/", (u32)max - 1);
        out[max - 1] = 0;
        return;
    }
    strcpy(out, tty_cwd);
    int l = (int)strlen(out);
    if (l && out[l - 1] != '/' && l + 1 < max) { out[l] = '/'; out[l + 1] = 0; }
    strncat(out, arg, (u32)(max - (int)strlen(out) - 1));
}

static void tty_procs(void)
{
    char row[112], n[16];
    tty_print("PID  NAME        TYPE    STATE      MEM");
    for (int i = 0; i < proc_sys_count(); i++) {
        strcpy(row, "     ");
        fmt_u32(n, (u32)i);
        strcpy(row, n);
        while ((int)strlen(row) < 5) strcat(row, " ");
        strncat(row, proc_sys_name(i), 11);
        while ((int)strlen(row) < 16) strcat(row, " ");
        strcat(row, "system  running  ");
        u32 kb = 0;
        int show = 0;
        if (i == 0) { kb = proc_kernel_mem_kb(); show = 1; }
        else if (i == 2) { kb = proc_wm_mem_kb(); show = 1; }
        else if (i == 3) { kb = vfs_usage_bytes() / 1024; show = 1; }
        if (show) { fmt_u32(n, kb); strcat(row, n); strcat(row, " KB"); }
        else strcat(row, "-");
        tty_print(row);
    }
    /* r37: app windows are a WM resource - with the WM down, listing
     * stale window structs as "running" would be a lie */
    if (!tty_wm_alive) {
        tty_print("(no app processes: the window manager is not running)");
        return;
    }
    int nw = wm_win_count();
    for (int i = 0; i < nw && i < 20; i++) {
        struct window *aw = wm_win_at(i);
        strcpy(row, "     ");
        fmt_u32(n, (u32)(10 + i));
        strcpy(row, n);
        while ((int)strlen(row) < 5) strcat(row, " ");
        strncat(row, aw->app ? aw->app->id : "?", 11);
        while ((int)strlen(row) < 16) strcat(row, " ");
        strcat(row, "app     ");
        strcat(row, aw->state == WIN_STATE_MIN ? "minimized  " : "running    ");
        fmt_u32(n, proc_win_mem_kb(aw));
        strcat(row, n); strcat(row, " KB");
        tty_print(row);
    }
}

/* the WM-binding failure every GUI-facing command reports when the
 * window manager is down - the exact semantics the field feedback
 * asked for ("app failed startup, cannot bind to wm") */
static void tty_no_wm(const char *cmd)
{
    char m[128];
    strcpy(m, cmd);
    strcat(m, ": app failed startup: cannot bind to wm");
    tty_print(m);
    tty_print("(the window manager is not running - 'wm' starts it; "
              "everything else here works without it)");
}

static struct shell_confirm tty_confirmation;
static void trace_tty_emit(const char *line, void *ctx) { (void)ctx; tty_print(line); }

static void tty_exec(char *cmd)
{
    char message[192];
    if (confirm_command(&tty_confirmation, cmd, TTY_COLS + 1, message)) { tty_print(message); return; }
    char *args[6];
    int nargs = 0;
    char *p = cmd;
    while (*p && nargs < 6) {
        while (*p == ' ') p++;
        if (!*p) break;
        args[nargs++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (!nargs) return;

    if (!strcmp(args[0], "help")) {
        tty_print(
            "commands (the full OS runs from this console; only GUI apps "
            "need the WM):\n"
            "  help            this list\n"
            "  ls [path]       list a directory (default: cwd)\n"
            "  cd <path>       change directory (cd .. / cd / go up, cd / = root)\n"
            "  pwd             print the working directory\n"
            "  cat <file>      print a file (e.g. cat system/boot.log)\n"
            "  mkdir <dir>     create a directory\n"
            "  touch <file>    create an empty file\n"
            "  rm [-s] <path>  delete a file/dir (-s forces system paths)\n"
            "  echo <text>     print text\n"
            "  calc <a> <op> <b>   arithmetic: + - * /\n"
            "  date / cal      clock / calendar\n"
            "  sysinfo         system information (scinfo = alias)\n"
            "  neofetch        the SCos system summary\n"
            "  theme [id]      show or switch the color theme\n"
            "  procs           system tasks + app windows with pids\n"
            "  kill <pid>      terminate an app process (pid >= 10, needs wm)\n"
            "  appstrt <app>   launch a GUI app (needs wm; binds you back to it)\n"
            "  wm              return to the desktop / start the WM\n"
            "  dmesg           kernel log ring (USB, input, fb, mm...)\n"
            "  diag [sub]      hardware diagnostics (all/pci/usb/input; bare\n"
            "                  'diag' opens the held diagnostics screen)\n"
            "  free            memory pool + allocator counters\n"
            "  disks           storage devices\n"
            "  sysrq <act>     panic/reboot/error/dump/time - system requests\n"
            "  ping            network reachability (there is no stack)\n"
            "  whoami/version  identity\n"
            "  clear           clear this console\n"
            "  reboot          reboot the machine now\n"
            "  inputtrace start|stop|show|save - USB recorder\n"
            "  kill --system <pid> - confirmed stop (scwm only)\n"
            "  shutdown [--confirm] - ACPI power-off");
    }
    else if (!strcmp(args[0], "ls")) {
        char path[160];
        tty_resolve(nargs > 1 ? args[1] : "", path, sizeof path);
        if (!strncmp(path, "/system", 7))
            tty_print("Warning: /system holds OS files - view with care, "
                      "edits can break SCos");
        struct vfs_node *nd = vfs_lookup(path);
        if (!nd || !vfs_is_dir(nd)) { tty_print("ls: invalid path"); return; }
        char names[64][VFS_NAME];
        int c = vfs_list(nd, names, 64);
        if (!c) tty_print("(empty directory)");
        for (int i = 0; i < c; i++) tty_print(names[i]);
    }
    else if (!strcmp(args[0], "cd")) {
        char target[160];
        if (nargs < 2) strcpy(target, "/home/");
        else if (!strcmp(args[1], "..")) {
            char tmp[160];
            vfs_parent_path(tty_cwd, tmp);
            strcpy(target, tmp);
        } else tty_resolve(args[1], target, sizeof target);
        int len = (int)strlen(target);
        if (len && target[len - 1] != '/') strcat(target, "/");
        struct vfs_node *n = vfs_lookup(target);
        if (n && vfs_is_dir(n)) {
            strncpy(tty_cwd, target, sizeof tty_cwd - 1);
            tty_cwd[sizeof tty_cwd - 1] = 0;
            char m[192];
            strcpy(m, "Changed directory to ");
            strcat(m, tty_cwd);
            tty_print(m);
        } else {
            char m[192];
            strcpy(m, "cd: directory not found: ");
            strncat(m, target, 150);
            tty_print(m);
        }
    }
    else if (!strcmp(args[0], "pwd")) tty_print(tty_cwd);
    else if (!strcmp(args[0], "cat")) {
        if (nargs < 2) { tty_print("cat: need a filename"); return; }
        char path[160];
        tty_resolve(args[1], path, sizeof path);
        if (!strncmp(path, "/system/", 8))
            tty_print("Warning: viewing a system file - do not edit "
                      "unless you know what it does");
        u32 len = 0;
        char *data = vfs_read(path, &len);
        if (!data) tty_print("cat: file not found");
        else tty_print(data);
    }
    else if (!strcmp(args[0], "mkdir")) {
        if (nargs < 2) { tty_print("mkdir: need a directory name"); return; }
        char path[160];
        tty_resolve(args[1], path, sizeof path);
        if (vfs_mkdir(path)) tty_print(path);
        else tty_print("mkdir: failed to create directory");
    }
    else if (!strcmp(args[0], "touch")) {
        if (nargs < 2) { tty_print("touch: need a filename"); return; }
        char path[160];
        tty_resolve(args[1], path, sizeof path);
        if (vfs_write(path, "", 0)) tty_print(path);
        else tty_print("touch: failed to create file");
    }
    else if (!strcmp(args[0], "rm")) {
        int force = 0, invalid = 0;
        const char *target = NULL;
        for (int j = 1; j < nargs; j++) {
            if (!strcmp(args[j], "-s") || !strcmp(args[j], "-f")) force = 1;
            else if (!strcmp(args[j], "-i")) { }
            else if (args[j][0]=='-' || target) invalid=1;
            else target = args[j];
        }
        if (invalid) { tty_print("Usage: rm [-i] [-s|-f] <path>; unknown flags/multiple paths rejected"); return; }
        if (!target) { tty_print("rm: need a path (rm [-s] <path>)"); return; }
        char path[160];
        tty_resolve(target, path, sizeof path);
        if (!force && !strncmp(path, "/system/", 8))
            tty_print("rm: system file - use 'rm -s <path>' to delete it "
                      "anyway");
        else if (vfs_delete(path)) {
            char m[192];
            strcpy(m, "Removed: ");
            strncat(m, path, 150);
            tty_print(m);
        } else tty_print("rm: file or directory not found");
    }
    else if (!strcmp(args[0], "echo")) {
        char m[TTY_COLS + 1];
        m[0] = 0;
        for (int j = 1; j < nargs; j++) {
            if (j > 1 && (int)strlen(m) < TTY_COLS - 1) strcat(m, " ");
            strncat(m, args[j], TTY_COLS - (int)strlen(m) - 1);
        }
        tty_print(m);
    }
    else if (!strcmp(args[0], "calc")) {
        if (nargs != 4) {
            tty_print("Usage: calc <number1> <operator> <number2>");
            return;
        }
        char *e1, *e2;
        double n1 = strtod_simple(args[1], &e1);
        double n2 = strtod_simple(args[3], &e2);
        if (*e1 || *e2) { tty_print("calc: invalid numbers"); return; }
        double r = 0;
        if (!strcmp(args[2], "+")) r = n1 + n2;
        else if (!strcmp(args[2], "-")) r = n1 - n2;
        else if (!strcmp(args[2], "*")) r = n1 * n2;
        else if (!strcmp(args[2], "/")) {
            if (n2 == 0) { tty_print("calc: division by zero"); return; }
            r = n1 / n2;
        } else { tty_print("calc: invalid operator (use + - * /)"); return; }
        char m[64];
        strcpy(m, "Result: ");
        fmt_double(m + strlen(m), r);
        tty_print(m);
    }
    else if (!strcmp(args[0], "date")) {
        static const char *const dn[] = { "Monday", "Tuesday", "Wednesday",
            "Thursday", "Friday", "Saturday", "Sunday" };
        static const char *const mn[] = { "January", "February", "March",
            "April", "May", "June", "July", "August", "September",
            "October", "November", "December" };
        struct rtc_time rt;
        rtc_read(&rt);
        char m[96], a[8], b[8];
        int wd = (rt.weekday >= 1 && rt.weekday <= 7) ? rt.weekday - 1 : 0;
        int mo = (rt.mon >= 1 && rt.mon <= 12) ? rt.mon - 1 : 0;
        strcpy(m, dn[wd]);
        strcat(m, " ");
        strncat(m, mn[mo], 12);
        strcat(m, " ");
        fmt_pad2(a, rt.day); strcat(m, a);
        strcat(m, " ");
        fmt_u32(b, (u32)rt.year); strcat(m, b);
        strcat(m, " ");
        fmt_pad2(a, rt.hour); strcat(m, a);
        strcat(m, ":");
        fmt_pad2(a, rt.min); strcat(m, a);
        strcat(m, ":");
        fmt_pad2(a, rt.sec); strcat(m, a);
        tty_print(m);
    }
    else if (!strcmp(args[0], "cal")) {
        static const char *const mn[] = { "January", "February", "March",
            "April", "May", "June", "July", "August", "September",
            "October", "November", "December" };
        struct rtc_time rt;
        rtc_read(&rt);
        char m[96], n[8];
        int mo = (rt.mon >= 1 && rt.mon <= 12) ? rt.mon - 1 : 0;
        strcpy(m, mn[mo]);
        strcat(m, " ");
        fmt_u32(n, (u32)rt.year);
        strcat(m, n);
        tty_print(m);
        tty_print("Su Mo Tu We Th Fr Sa");
        static const u8 md[12] = { 31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31 };
        int days = md[mo];
        if (rt.mon == 2 &&
            ((rt.year % 4 == 0 && rt.year % 100 != 0) || rt.year % 400 == 0))
            days = 29;
        int wd1 = (rt.weekday - (rt.day - 1) % 7 + 7) % 7;
        char row[32];
        row[0] = 0;
        for (int i = 0; i < wd1; i++) strcat(row, "   ");
        for (int d = 1; d <= days; d++) {
            char cell[8];
            if (d == rt.day) {
                cell[0] = '[';
                fmt_u32(cell + 1, (u32)d);
                strcat(cell, "]");
            } else {
                fmt_u32(cell, (u32)d);
                if (d < 10) { cell[1] = cell[0]; cell[0] = ' '; cell[2] = 0; }
            }
            strcat(row, cell);
            strcat(row, (d == rt.day) ? "" : " ");
            if ((wd1 + d) % 7 == 0) { tty_print(row); row[0] = 0; }
        }
        if (row[0]) tty_print(row);
    }
    else if (!strcmp(args[0], "sysinfo") || !strcmp(args[0], "scinfo")) {
        char m[96], n[16];
        struct rtc_time rt;
        rtc_read(&rt);
        tty_print("SCos System Information:");
        tty_print("OS Version: 2.0.0 (build " SCOS_BUILD_TAG ")");
        tty_print("Kernel: sckern (SCos 2.0.0)");
        tty_print("Architecture: x86 (32-bit protected mode)");
        strcpy(m, "Uptime: ");
        fmt_u32(n, uptime_ms() / 60000);
        strcat(m, n); strcat(m, " minutes");
        tty_print(m);
        strcpy(m, "Total Memory: ");
        fmt_u32(n, mm_total_kb() / 1024);
        strcat(m, n); strcat(m, " MB");
        tty_print(m);
        strcpy(m, "Free Memory: ");
        fmt_u32(n, mm_free_kb() / 1024);
        strcat(m, n); strcat(m, " MB");
        tty_print(m);
        strcpy(m, "CPU Cores: ");
        fmt_u32(n, cpu_core_count());
        strcat(m, n); strcat(m, " (");
        fmt_u32(n, cpu_thread_count());
        strcat(m, n); strcat(m, " threads)");
        tty_print(m);
        strcpy(m, "Storage Used: ");
        fmt_u32(n, vfs_usage_bytes() / 1024);
        strcat(m, n); strcat(m, " KB");
        tty_print(m);
        strcpy(m, "Disk: ");
        strcat(m, ata_present() ? "ATA present" : "none");
        if (fs_image_found) strcat(m, " (SCos image loaded)");
        tty_print(m);
        char cpu[49];
        cpu_brand(cpu, sizeof cpu);
        if (cpu[0]) {
            strcpy(m, "CPU Model: ");
            strncat(m, cpu, 48);
            tty_print(m);
            strcpy(m, "Speed: ");
            fmt_u32(n, cpu_mhz());
            strcat(m, n); strcat(m, " MHz (TSC-measured)");
            tty_print(m);
        }
        char ul[96];
        usb_status(ul, sizeof ul);
        tty_print(ul);
        tty_print("Window manager: ");
        tty_print(tty_wm_alive ? "  running (this console overlays it)"
                               : "  NOT RUNNING - rescue mode");
    }
    else if (!strcmp(args[0], "neofetch")) {
        for (int i = 0; i < neofetch_art_lines; i++)
            tty_print(neofetch_art[i]);
        char m[96], n[16];
        char cpu[49];
        cpu_brand(cpu, sizeof cpu);
        if (!cpu[0]) strcpy(cpu, "unknown x86 processor");
        u32 tot = 0, fre = 0;
        mm_stats(&tot, &fre);
        tty_print("user@scos");
        tty_print("---------------------");
        tty_print("OS:      SCos 2.0.0 (build " SCOS_BUILD_TAG ")");
        strcpy(m, "Kernel:  sckern");
        tty_print(m);
        strcpy(m, "Uptime:  ");
        fmt_u32(n, uptime_ms() / 60000);
        strcat(m, n); strcat(m, " minutes");
        tty_print(m);
        strcpy(m, "Memory:  ");
        fmt_u32(n, tot - fre);
        strcat(m, n); strcat(m, " KB / ");
        fmt_u32(n, tot);
        strcat(m, n); strcat(m, " KB");
        tty_print(m);
        strcpy(m, "CPU:     ");
        strncat(m, cpu, 40);
        tty_print(m);
        strcpy(m, "MHz:     ");
        fmt_u32(n, cpu_mhz());
        strcat(m, n);
        tty_print(m);
        strcpy(m, "Disk:    ");
        fmt_u32(n, vfs_usage_bytes() / 1024);
        strcat(m, n); strcat(m, " KB used (vfs)");
        tty_print(m);
        strcpy(m, "WM:      ");
        strcat(m, tty_wm_alive ? "scwm (running)" : "DOWN (rescue tty)");
        tty_print(m);
        strcpy(m, "Theme:   ");
        strncat(m, theme_current()->name, 32);
        tty_print(m);
    }
    else if (!strcmp(args[0], "theme")) {
        if (nargs < 2) {
            char m[128];
            strcpy(m, "Current theme: ");
            strcat(m, theme_current()->name);
            tty_print(m);
            tty_print("Available:");
            for (int i = 0; i < theme_count(); i++) {
                strcpy(m, "  ");
                strcat(m, theme_get(i)->id);
                strcat(m, "  - ");
                strncat(m, theme_get(i)->name, 60);
                tty_print(m);
            }
            tty_print("Usage: theme <id>");
            return;
        }
        int found = -1;
        for (int i = 0; i < theme_count(); i++)
            if (!strcmp(theme_get(i)->id, args[1])) found = i;
        if (found < 0) {
            char m[96];
            strcpy(m, "theme: unknown theme: ");
            strncat(m, args[1], 40);
            tty_print(m);
            return;
        }
        theme_set_index(found);
        settings_save();
        if (tty_wm_alive) wm_theme_changed();
        char m[96];
        strcpy(m, "Theme switched to ");
        strcat(m, theme_current()->name);
        tty_print(m);
    }
    else if (!strcmp(args[0], "procs")) tty_procs();
    else if (!strcmp(args[0], "inputtrace")) {
        usb_inputtrace(nargs == 1 ? "show" : nargs == 2 ? args[1] : "--help", trace_tty_emit, NULL);
    }
    else if (!strcmp(args[0], "kill")) {
        int pid = -1;
        int system = nargs == 3 && !strcmp(args[1], "--system");
        if (nargs == 2 && !strcmp(args[1], "--help")) {
            tty_print("kill <pid> | kill --system <pid>; only scwm (2) is independently stoppable"); return;
        }
        if ((!system && nargs != 2) || !parse_pid(args[system ? 2 : 1], &pid)) {
            tty_print("Usage: kill <pid> | kill --system <pid>; decimal PID required"); return;
        }
        if (system) {
            if (pid == 2 && tty_wm_alive) { wm_stop_requested = 1; tty_exit = 1; }
            else tty_print("No independent stop operation available; nothing terminated.");
            return;
        }
        if (pid >= 0 && pid < proc_sys_count()) {
            tty_print("kill: cannot kill a system task from the console");
            return;
        }
        if (!tty_wm_alive) { tty_no_wm("kill"); return; }
        if (pid >= 10 && pid - 10 < wm_win_count()) {
            struct window *v = wm_win_at(pid - 10);
            if (v->app && v->app->id[0] == '_')
                tty_print("kill: cannot kill a system dialog");
            else {
                wm_close_window(v);
                char m[80];
                strcpy(m, "kill: terminated pid ");
                strcat(m, args[1]);
                tty_print(m);
            }
        } else tty_print("kill: no such pid");
    }
    else if (!strcmp(args[0], "appstrt") || !strcmp(args[0], "open")) {
        if (nargs < 2) {
            tty_print("appstrt <app>: about blackjack browser calendar "
                      "files notepad settings solitaire sysmon terminal");
            return;
        }
        /* r37: GUI apps are the ONE thing this console cannot do without
         * the WM - fail loudly and correctly instead of half-launching */
        if (!tty_wm_alive) { tty_no_wm("appstrt"); return; }
        if (wm_restarts >= 5) {
            tty_print("appstrt: the WM keeps exiting - GUI apps cannot "
                      "run; use 'reboot' for a clean start");
            return;
        }
        struct window *nw = wm_open_app(args[1], NULL);
        if (!nw) { tty_print("appstrt: unknown app (see 'help')"); return; }
        tty_print("appstrt: launched - returning you to the desktop");
        tty_exit = 1;
    }
    else if (!strcmp(args[0], "wm") || !strcmp(args[0], "exit")) {
        if (!tty_wm_alive && wm_restarts >= 5) {
            tty_print("wm: restart budget exhausted (the WM keeps dying) "
                      "- staying in the console; 'reboot' for a clean start");
            return;
        }
        tty_exit = 1;
    }
    else if (!strcmp(args[0], "dmesg") || !strcmp(args[0], "klog")) {
        int c = klog_ring_count();
        char out[200];
        int start = nargs > 1 && !strcmp(args[1], "-t") && c > 14 ? c - 14 : 0;
        for (int i = start; i < c; i++) {
            if (!klog_ring(i, out, (int)sizeof(out))) break;
            tty_print(out);
        }
    }
    else if (!strcmp(args[0], "diag")) {
        const char *sub = nargs > 1 ? args[1] : NULL;
        if (!sub || !strcmp(sub, "all") || !strcmp(sub, "hold")) {
            tty_print("diag: opening the diagnostics screen (any key "
                      "returns here, 30 s max)");
            diag_run();
            return;
        }
        char out[200];
        if (!strcmp(sub, "pci")) pci_scan_dump();
        else if (!strcmp(sub, "usb")) {
            char ul[96];
            usb_status(ul, sizeof ul);
            klog("diag: %s", ul);
        } else if (!strcmp(sub, "input")) {
            klog("diag: ps/2 mouse %s", mouse_present() ? "present"
                                                        : "absent");
            klog("diag: input %s",
                 input_last_tick ? "events seen" : "silent");
        } else {
            tty_print("diag: unknown subsystem - use pci, usb, input, all");
            return;
        }
        int c = klog_ring_count();
        int start = c > 14 ? c - 14 : 0;
        for (int i = start; i < c; i++) {
            if (!klog_ring(i, out, (int)sizeof(out))) break;
            tty_print(out);
        }
    }
    else if (!strcmp(args[0], "free")) {
        u32 tot = 0, fre = 0, a = 0, f = 0;
        mm_stats(&tot, &fre);
        mm_ops(&a, &f);
        char m[96], n[16];
        strcpy(m, "memory: total ");
        fmt_u32(n, tot); strcat(m, n); strcat(m, " KB, free ");
        fmt_u32(n, fre); strcat(m, n); strcat(m, " KB, used ");
        fmt_u32(n, tot - fre); strcat(m, n); strcat(m, " KB");
        tty_print(m);
        strcpy(m, "allocator: ");
        fmt_u32(n, a); strcat(m, n); strcat(m, " allocs, ");
        fmt_u32(n, f); strcat(m, n); strcat(m, " frees");
        tty_print(m);
    }
    else if (!strcmp(args[0], "disks")) {
        tty_print(ata_present() ? "ata0: ATA disk present"
                                : "ata0: no ATA disk detected");
        tty_print(fs_image_found ? "fs:   SCos disk image loaded"
                                 : "fs:   no SCos disk image (RAM vfs)");
        char m[80], n[16];
        strcpy(m, "vfs:  ");
        fmt_u32(n, vfs_usage_bytes() / 1024);
        strcat(m, n); strcat(m, " KB used");
        tty_print(m);
    }
    else if (!strcmp(args[0], "sysrq")) {
        if (nargs < 2) {
            tty_print(
                "sysrq - system request key. Actions:\n"
                "  sysrq panic [msg]  trigger a real kernel panic\n"
                "  sysrq reboot       reboot the machine now\n"
                "  sysrq error        show the non-fatal error screen "
                "(self-test)\n"
                "  sysrq dump         dump recent kernel log here\n"
                "  sysrq time         PIT uptime, tick rate and RTC clock");
            return;
        }
        if (!strcmp(args[1], "panic")) {
            char msg[128];
            msg[0] = 0;
            for (int j = 2; j < nargs; j++) {
                if (j > 2) strcat(msg, " ");
                strncat(msg, args[j], 96);
            }
            tty_print("Triggering kernel panic as requested...");
            tty_draw();
            sleep_ms(600);
            kernel_panic(msg[0] ? msg : "panic requested via sysrq");
            return;
        }
        if (!strcmp(args[1], "reboot")) {
            tty_print("Rebooting SCos... Please wait.");
            tty_draw();
            cpu_reboot_8042();
            return;
        }
        if (!strcmp(args[1], "error")) {
            static const char *dump[3] = {
                "example: last io port 0x3f6 status 0x50",
                "example: retry count 3, drive master",
                "example: caller vfs_flush() + 0x2a",
            };
            err_notify("self-test",
                       "this is what a non-fatal subsystem error looks "
                       "like - presented by the console itself (no WM "
                       "needed)", dump, 3);
            err_show_pending();   /* show it right now, WM or no WM */
            tty_print("Error screen dismissed - system continued");
            return;
        }
        if (!strcmp(args[1], "dump")) {
            int n = klog_ring_count();
            int start = n > 14 ? n - 14 : 0;
            char ln[200];
            for (int i = start; i < n; i++)
                if (klog_ring(i, ln, (int)sizeof(ln))) tty_print(ln);
            if (!n) tty_print("(log empty)");
            return;
        }
        if (!strcmp(args[1], "time")) {
            struct rtc_time rt;
            rtc_read(&rt);
            char m[96], a[16], b[8], c[8];
            fmt_u32(a, uptime_ms());
            strcpy(m, "uptime: ");
            strcat(m, a);
            strcat(m, " ms (PIT ticks)");
            tty_print(m);
            fmt_pad2(b, rt.hour);
            fmt_pad2(c, rt.min);
            strcpy(m, "rtc:    ");
            strcat(m, b); strcat(m, ":"); strcat(m, c);
            strcat(m, " local CMOS clock");
            tty_print(m);
            return;
        }
        char m[96];
        strcpy(m, "sysrq: unknown action '");
        strncat(m, args[1], 32);
        strcat(m, "' - run 'sysrq' for the list");
        tty_print(m);
    }
    else if (!strcmp(args[0], "ping")) {
        tty_print("ping: this kernel has no TCP/IP stack - network "
                  "unreachable.");
        tty_print("No network interface driver is present (see 'disks'/"
                  "'sysinfo' for real hardware).");
    }
    else if (!strcmp(args[0], "whoami")) tty_print("user");
    else if (!strcmp(args[0], "version"))
        tty_print("SCos version 2.0.0 (build " SCOS_BUILD_TAG
                  ") - kernel console v2.0");
    else if (!strcmp(args[0], "make")) {
        /* inside joke from the os.html days - deliberately not in help */
        if (nargs == 2 && !strcmp(args[1], "real")) tty_print("real!!!");
        else tty_print("make: nothing to be done");
    }
    else if (!strcmp(args[0], "clear")) tty_nlines = 0;
    else if (!strcmp(args[0], "reboot")) {
        if (nargs > 1 && (nargs != 2 || strcmp(args[1],"--confirm"))) { tty_print("Usage: reboot [--confirm]"); return; }
        tty_print("rebooting...");
        tty_draw();
        cpu_reboot_8042();
    }
    else if (!strcmp(args[0], "shutdown") || !strcmp(args[0], "poweroff")) {
        if (nargs > 1 && (nargs != 2 || strcmp(args[1],"--confirm"))) { tty_print("Usage: shutdown [--confirm]"); return; }
        tty_print("powering off...");
        tty_draw();
        if (acpi_shutdown()) { wm_poweroff_screen(); for (;;) cpu_hlt(); }
        tty_print("shutdown: no ACPI power-off on this machine - hold the "
                  "power button");
    }
    else {
        char m[TTY_COLS + 16];
        strcpy(m, "unknown command: ");
        strncat(m, args[0], TTY_COLS - 20);
        strcat(m, "  ('help')");
        tty_print(m);
    }
}

static void tty_console_loop(void)
{
    tty_exit = 0;
    tty_input[0] = 0;
    tty_ipos = 0;
    tty_draw();
    while (!tty_exit) {
        usb_poll();                    /* the keyboard is USB - pump it */
        /* r37: error screens no longer depend on the WM - while this
         * console owns the screen IT presents pending non-fatal errors
         * (error_screen draws + holds by itself, then returns here) */
        if (err_pending()) {
            err_show_pending();
            tty_draw();
        }
        struct key_event ke;
        int got = 0;
        while (kbd_poll(&ke)) {
            got = 1;
            if (!ke.pressed) continue;
            if (tty_confirmation.command[0] && (ke.keycode==3 || ke.keycode==27)) {
                tty_confirmation.command[0]=0; tty_input[0]=0; tty_ipos=0;
                tty_print("Cancelled."); tty_draw(); continue;
            }
            /* r37: Ctrl+Alt+F7 - Linux's "back to the GUI VT" switch;
             * the WM side is Ctrl+Alt+F1 (see wm.c handle_key) */
            if (ke.ctrl && ke.alt && ke.keycode == KEY_F7) {
                tty_exit = 1;
                break;
            }
            if (ke.keycode == '\n') {
                char echo[TTY_COLS + 16];
                tty_prompt(echo, TTY_COLS - 2);
                strncat(echo, tty_input,
                        TTY_COLS + 14 - (int)strlen(echo));
                tty_print(echo);
                char cmd[TTY_COLS + 1];
                strcpy(cmd, tty_input);
                tty_input[0] = 0;
                tty_ipos = 0;
                tty_exec(cmd);         /* may set tty_exit */
                tty_draw();
            } else {
                edit_line(tty_input, &tty_ipos, (int)sizeof(tty_input), &ke);
                tty_draw();
            }
        }
        if (!got) cpu_hlt();
    }
}

void tty_run(int return_to_wm)
{
    tty_wm_alive = return_to_wm;
    tty_rows = (screen_h - 8) / (FONT_H + 2);
    if (tty_rows > TTY_ROWS_MAX) tty_rows = TTY_ROWS_MAX;
    if (tty_rows < 4) tty_rows = 4;
    klog("tty: maintenance console entered (wm %s)",
         return_to_wm ? "alive - manual entry" : "EXITED - rescue mode");

    for (;;) {
        tty_nlines = 0;
        tty_print("SCos maintenance console (build " SCOS_BUILD_TAG
                  ") - kernel-owned rescue tty");
        if (return_to_wm)
            tty_print("the window manager is alive; 'wm' returns to the "
                      "desktop");
        else {
            tty_print("THE WINDOW MANAGER HAS EXITED - the desktop is "
                      "gone.");
            tty_print("'wm' restarts it; everything else here works "
                      "without it.");
        }
        tty_print("this console runs the whole OS (Linux model): every "
                  "command works except GUI apps, which need the wm");
        tty_print("'wm' or ctrl+alt+f7 returns to the desktop (ctrl+"
                  "alt+f1 brings you back here)");
        tty_print("type 'help' for commands");
        tty_console_loop();
        if (return_to_wm) return;      /* the WM loop reclaims its state */
        /* rescue mode: 'wm'/'appstrt' asked for the desktop back */
        if (wm_restarts >= 5) {        /* loop should have refused; belt */
            tty_console_loop();
            continue;
        }
        wm_restarts++;
        klog("tty: restarting the WM (attempt %d of 5)", wm_restarts);
        wm_run();                      /* nested; may return again */
        klog("tty: wm_run RETURNED again (crash loop?) - back to console");
    }
}
