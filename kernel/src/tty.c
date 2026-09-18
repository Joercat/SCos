/* SCos native - kernel maintenance console ("tty").
 *
 * r36 feature (field request): a Linux-tty-style rescue shell owned by
 * the KERNEL, independent of the window manager.  Entry points:
 *   - the terminal command `tty` (raises tty_request; the WM main loop
 *     hands screen + keyboard over and reclaims them when tty returns),
 *   - wm_run() ever RETURNING (the WM died - kmain falls through here),
 * so a system whose compositor crashed or wedged still has a real text
 * interface: inspect the filesystem, read the kernel log, list and kill
 * app processes, launch apps, run diagnostics, reboot, shut down.
 *
 * Like everything else in SCos this is polled: each loop pumps
 * usb_poll() (the keyboard is a USB device - without this the console
 * itself would be deaf), drains kbd_poll(), edits the line with the
 * shared edit_line() helper and repaints the 8x16 text grid.
 */
#include "scos.h"

int tty_request;                     /* raised by the terminal `tty` cmd */

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
    strcpy(pl, "> ");
    strncat(pl, tty_input, TTY_COLS);
    s_text(&screen, 6, y, pl, TTY_PROMPT_FG);
    /* block cursor after the input position */
    s_char_bg(&screen, 6 + (2 + tty_ipos) * FONT_W, y, ' ', TTY_BG, TTY_FG);
    fb_flip();
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

static void tty_exec(char *cmd)
{
    char *args[4];
    int nargs = 0;
    char *p = cmd;
    while (*p && nargs < 4) {
        while (*p == ' ') p++;
        if (!*p) break;
        args[nargs++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (!nargs) return;

    if (!strcmp(args[0], "help")) {
        tty_print(
            "commands:\n"
            "  help            this list\n"
            "  ls [path]       list a directory (default /)\n"
            "  cat <file>      print a file (e.g. cat system/boot.log)\n"
            "  procs           system tasks + app windows with pids\n"
            "  kill <pid>      terminate an app process (pid >= 10)\n"
            "  appstrt <app>   launch an app (hands you back to the WM)\n"
            "  wm              return to the desktop / restart the WM\n"
            "  dmesg           kernel log ring (USB, input, fb, mm...)\n"
            "  diag            full hardware diagnostics screen\n"
            "  free            memory pool + allocator counters\n"
            "  clear           clear this console\n"
            "  reboot          reboot the machine now\n"
            "  shutdown        ACPI power-off");
    }
    else if (!strcmp(args[0], "ls")) {
        const char *path = nargs > 1 ? args[1] : "/";
        struct vfs_node *nd = vfs_lookup(path);
        if (!nd || !vfs_is_dir(nd)) { tty_print("ls: invalid path"); return; }
        char names[64][VFS_NAME];
        int c = vfs_list(nd, names, 64);
        if (!c) tty_print("(empty directory)");
        for (int i = 0; i < c; i++) tty_print(names[i]);
    }
    else if (!strcmp(args[0], "cat")) {
        if (nargs < 2) { tty_print("cat: need a filename"); return; }
        u32 len = 0;
        char *data = vfs_read(args[1], &len);
        if (!data) tty_print("cat: file not found");
        else tty_print(data);
    }
    else if (!strcmp(args[0], "procs")) tty_procs();
    else if (!strcmp(args[0], "kill")) {
        if (nargs < 2) { tty_print("kill: need a pid (see 'procs')"); return; }
        int pid = (int)str_to_u32(args[1]);
        if (pid >= 0 && pid < proc_sys_count())
            tty_print("kill: cannot kill a system task from the console");
        else if (pid >= 10 && pid - 10 < wm_win_count()) {
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
        if (!tty_wm_alive && wm_restarts >= 5) {
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
    else if (!strcmp(args[0], "dmesg")) {
        int c = klog_ring_count();
        char out[200];
        for (int i = 0; i < c; i++) {
            if (!klog_ring(i, out, (int)sizeof(out))) break;
            tty_print(out);
        }
    }
    else if (!strcmp(args[0], "diag")) {
        tty_print("diag: opening the diagnostics screen (any key returns "
                  "here, 30 s max)");
        diag_run();
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
    else if (!strcmp(args[0], "clear")) tty_nlines = 0;
    else if (!strcmp(args[0], "reboot")) {
        tty_print("rebooting...");
        tty_draw();
        cpu_reboot_8042();
    }
    else if (!strcmp(args[0], "shutdown") || !strcmp(args[0], "poweroff")) {
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
        struct key_event ke;
        int got = 0;
        while (kbd_poll(&ke)) {
            got = 1;
            if (!ke.pressed) continue;
            if (ke.keycode == '\n') {
                char echo[TTY_COLS + 4];
                strcpy(echo, "> ");
                strncat(echo, tty_input, TTY_COLS);
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
        tty_print("SCos maintenance console (build r36) - kernel-owned "
                  "rescue tty");
        if (return_to_wm)
            tty_print("the window manager is alive; 'wm' returns to the "
                      "desktop");
        else {
            tty_print("THE WINDOW MANAGER HAS EXITED - the desktop is "
                      "gone.");
            tty_print("'wm' restarts it; everything else here works "
                      "without it.");
        }
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
