/*
 * SCos native - Terminal app.
 *
 * Faithful port of the web simulation's terminal: same command set and
 * behaviour (including the typewriter output effect and the simulated
 * network ping driven by system/network.json).
 */
#include "scos.h"

#define TERM_LINES 512
#define TERM_LINE 256
#define HIST 16

struct term {
    char lines[TERM_LINES][TERM_LINE];
    int nlines;
    int scroll;              /* first visible line; -1 = follow bottom */
    char input[TERM_LINE];
    int ipos;
    char hist[HIST][TERM_LINE];
    int hcount, hindex;
    char cwd[128];
    /* typewriter effect */
    char pending[2048];
    int pend_len, pend_shown, pending_active;
    /* simulated ping */
    int ping_active;
    char ping_host[64];
    int ping_seq;
    u64 ping_next;
    char aliases[8][2][64];
    int nalias;
    int shutting_down;
};

static void term_push_line(struct term *t, const char *s)
{
    /* split on \n */
    const char *p = s;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n') nl++;
        int len = (int)(nl - p);
        if (len >= TERM_LINE) len = TERM_LINE - 1;
        if (t->nlines < TERM_LINES) {
            memcpy(t->lines[t->nlines], p, len);
            t->lines[t->nlines][len] = 0;
            t->nlines++;
        } else {
            for (int i = 1; i < TERM_LINES; i++)
                memcpy(t->lines[i - 1], t->lines[i], TERM_LINE);
            memcpy(t->lines[TERM_LINES - 1], p, len);
            t->lines[TERM_LINES - 1][len] = 0;
        }
        if (!*nl) break;
        p = nl + 1;
    }
}

static void term_print(struct term *t, const char *s)   /* immediate */
{
    term_push_line(t, s);
}

static void term_type(struct term *t, const char *s)    /* typewriter */
{
    strncpy(t->pending, s, sizeof(t->pending) - 1);
    t->pending[sizeof(t->pending) - 1] = 0;
    t->pend_len = (int)strlen(t->pending);
    t->pend_shown = 0;
    t->pending_active = 1;
}

static void prompt_str(struct term *t, char *out)
{
    /* mirror the web simulation: home shows as "~" */
    char tmp[160];
    strcpy(tmp, t->cwd);
    int l = (int)strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    char dispbuf[160];
    const char *disp = tmp;
    if (!strcmp(tmp, "/home")) disp = "~";
    else if (!strncmp(tmp, "/home/", 6)) {
        dispbuf[0] = '~';
        strcpy(dispbuf + 1, tmp + 5);
        disp = dispbuf;
    }
    strcpy(out, "user@scos:");
    strcat(out, disp);
    strcat(out, "$ ");
}

/* ------------------------------------------------------------ commands --- */
static const char *help_text =
    "Available commands:\n"
    "help     - Show this help message\n"
    "ls       - List directory contents\n"
    "cd       - Change directory\n"
    "cat      - Show file contents\n"
    "echo     - Display a message\n"
    "clear    - Clear terminal screen\n"
    "date     - Show current date and time\n"
    "mkdir    - Create directory\n"
    "touch    - Create an empty file\n"
    "rm       - Delete file or directory\n"
    "whoami   - Show current user\n"
    "version  - Show system version\n"
    "calc     - Perform basic arithmetic\n"
    "ping     - Ping a host\n"
    "sysinfo  - Display system information\n"
    "alias    - Create command aliases\n"
    "history  - Show command history\n"
    "open     - Launch a desktop app\n"
    "save     - Write the filesystem image to disk\n"
    "save     - Persist filesystem to disk (explicit, see docs)\n"
    "shutdown - Shut down the system\n"
    "reboot   - Reboot the system";

static const char *month_names[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *day_names[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

static void fmt_size(char *out, u32 bytes)
{
    if (bytes < 1024) { fmt_u32(out, bytes); strcat(out, " bytes"); }
    else if (bytes < 1024 * 1024) {
        fmt_u32(out, bytes / 1024); strcat(out, "."); fmt_u32(out + strlen(out), (bytes % 1024) * 100 / 1024);
        strcat(out, " KB");
    } else {
        fmt_u32(out, bytes / (1024 * 1024)); strcat(out, ".");
        fmt_u32(out + strlen(out), (bytes % (1024 * 1024)) * 100 / (1024 * 1024));
        strcat(out, " MB");
    }
}

static void resolve_path(struct term *t, const char *arg, char *out)
{
    if (arg[0] == '/') strcpy(out, arg);
    else {
        strcpy(out, t->cwd);
        int l = (int)strlen(out);
        if (l && out[l - 1] != '/') { out[l] = '/'; out[l + 1] = 0; }
        strcat(out, arg);
    }
}

static void start_ping(struct term *t, const char *host)
{
    u32 len = 0;
    char *json = vfs_read("system/network.json", &len);
    char needle[80];
    strcpy(needle, "\""); strcat(needle, host); strcat(needle, "\"");
    if (!json || !str_str(json, needle)) {
        char msg[128];
        strcpy(msg, "ping: cannot resolve "); strcat(msg, host); strcat(msg, ": Unknown host");
        term_type(t, msg);
        return;
    }
    t->ping_active = 1;
    strncpy(t->ping_host, host, sizeof(t->ping_host) - 1);
    t->ping_seq = 0;
    t->ping_next = tick_count + 2;
    char head[128];
    strcpy(head, "PING "); strcat(head, host); strcat(head, ": 56 data bytes");
    term_print(t, head);
}

static void run_command(struct term *t, const char *command)
{
    char cmdbuf[TERM_LINE];
    strncpy(cmdbuf, command, sizeof(cmdbuf) - 1);
    cmdbuf[sizeof(cmdbuf) - 1] = 0;

    char prompt[160];
    prompt_str(t, prompt);
    char echo[TERM_LINE + 160];
    strcpy(echo, prompt); strcat(echo, cmdbuf);
    term_print(t, echo);

    if (cmdbuf[0]) {
        if (t->hcount < HIST) strcpy(t->hist[t->hcount++], cmdbuf);
        else {
            for (int i = 1; i < HIST; i++) strcpy(t->hist[i - 1], t->hist[i]);
            strcpy(t->hist[HIST - 1], cmdbuf);
        }
        t->hindex = t->hcount;
    }

    /* alias expansion */
    char effective[TERM_LINE];
    strcpy(effective, cmdbuf);
    char *sp = effective;
    while (*sp == ' ') sp++;
    char first[64];
    int i = 0;
    while (sp[i] && sp[i] != ' ' && i < 63) { first[i] = sp[i]; i++; }
    first[i] = 0;
    for (int a = 0; a < t->nalias; a++)
        if (!strcmp(t->aliases[a][0], first)) {
            char rest[TERM_LINE];
            strcpy(rest, sp + i);
            strcpy(effective, t->aliases[a][1]);
            strcat(effective, rest);
            break;
        }

    /* tokenize (naive, space separated like the web version) */
    char *args[24];
    int nargs = 0;
    char *p = effective;
    while (*p && nargs < 24) {
        while (*p == ' ') p++;
        if (!*p) break;
        args[nargs++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (!nargs) return;

    const char *cmd = args[0];
    char response[1536];
    response[0] = 0;
    int typed = 1;

    if (!strcmp(cmd, "help")) strcpy(response, help_text);
    else if (!strcmp(cmd, "ls")) {
        char path[256];
        resolve_path(t, nargs > 1 ? args[1] : "", path);
        struct vfs_node *n = vfs_lookup(path);
        if (!n || !vfs_is_dir(n)) strcpy(response, "Error: Invalid path or permission denied.");
        else {
            char names[64][VFS_NAME];
            int c = vfs_list(n, names, 64);
            if (!c) strcpy(response, "(empty directory)");
            else {
                for (int j = 0; j < c; j++) {
                    if (j) strcat(response, "  ");
                    strcat(response, names[j]);
                }
            }
        }
    }
    else if (!strcmp(cmd, "cd")) {
        char target[256];
        if (nargs < 2) strcpy(target, "/home/");
        else if (!strcmp(args[1], "..")) {
            char tmp[256];
            vfs_parent_path(t->cwd, tmp);
            strcpy(target, tmp);
        } else resolve_path(t, args[1], target);
        int len = (int)strlen(target);
        if (len && target[len - 1] != '/') strcat(target, "/");
        struct vfs_node *n = vfs_lookup(target);
        if (n && vfs_is_dir(n)) {
            strcpy(t->cwd, target);
            strcpy(response, "Changed directory to "); strcat(response, target);
        } else {
            strcpy(response, "Error: Directory not found: "); strcat(response, target);
        }
    }
    else if (!strcmp(cmd, "cat")) {
        if (nargs < 2) strcpy(response, "Error: No filename specified");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            u32 len = 0;
            char *data = vfs_read(path, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                strncpy(response, data, sizeof(response) - 1);
                response[sizeof(response) - 1] = 0;
            }
        }
    }
    else if (!strcmp(cmd, "echo")) {
        for (int j = 1; j < nargs; j++) {
            if (j > 1) strcat(response, " ");
            strcat(response, args[j]);
        }
    }
    else if (!strcmp(cmd, "clear")) {
        t->nlines = 0;
        t->scroll = -1;
        return;
    }
    else if (!strcmp(cmd, "date")) {
        struct rtc_time rt;
        rtc_read(&rt);
        char a[4], b[4], c[4], d[8];
        fmt_pad2(a, rt.hour); fmt_pad2(b, rt.min); fmt_pad2(c, rt.sec);
        fmt_u32(d, rt.year);
        strcpy(response, day_names[(rt.weekday ? rt.weekday : 7) - 1]);
        strcat(response, " "); strcat(response, month_names[rt.mon - 1]);
        strcat(response, " "); fmt_pad2(a, rt.day); strcat(response, a);
        strcat(response, " "); strcat(response, d);
        strcat(response, " "); fmt_pad2(a, rt.hour); strcat(response, a);
        strcat(response, ":"); fmt_pad2(a, rt.min); strcat(response, a);
        strcat(response, ":"); fmt_pad2(a, rt.sec); strcat(response, a);
        strcat(response, " UTC");
    }
    else if (!strcmp(cmd, "mkdir")) {
        if (nargs < 2) strcpy(response, "Error: No directory name specified");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            if (vfs_mkdir(path)) {
                strcpy(response, "Directory created: "); strcat(response, path); strcat(response, "/");
            } else strcpy(response, "Error: Failed to create directory");
        }
    }
    else if (!strcmp(cmd, "touch")) {
        if (nargs < 2) strcpy(response, "Error: No filename specified");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            if (vfs_write(path, "", 0)) {
                strcpy(response, "File created: "); strcat(response, path);
            } else strcpy(response, "Error: Failed to create file");
        }
    }
    else if (!strcmp(cmd, "rm")) {
        if (nargs < 2) strcpy(response, "Error: No file or directory specified.");
        else {
            char path[256];
            resolve_path(t, args[1], path);
            if (vfs_delete(path)) {
                strcpy(response, "Removed: "); strcat(response, path);
            } else strcpy(response, "Error: File or directory not found.");
        }
    }
    else if (!strcmp(cmd, "whoami")) strcpy(response, "user");
    else if (!strcmp(cmd, "version")) strcpy(response, "SCos version 2.0.0, Terminal v2.0 (native)");
    else if (!strcmp(cmd, "calc")) {
        if (nargs != 4) strcpy(response, "Usage: calc <number1> <operator> <number2>");
        else {
            char *e1, *e2;
            double n1 = strtod_simple(args[1], &e1);
            double n2 = strtod_simple(args[3], &e2);
            if (*e1 || *e2) strcpy(response, "Error: Invalid numbers. Please use numeric values.");
            else {
                double r = 0;
                int ok = 1;
                if (!strcmp(args[2], "+")) r = n1 + n2;
                else if (!strcmp(args[2], "-")) r = n1 - n2;
                else if (!strcmp(args[2], "*")) r = n1 * n2;
                else if (!strcmp(args[2], "/")) {
                    if (n2 == 0) { strcpy(response, "Error: Division by zero is not allowed."); ok = 0; }
                    else r = n1 / n2;
                } else { strcpy(response, "Error: Invalid operator. Use +, -, *, or /. Check \"help calc\""); ok = 0; }
                if (ok) {
                    strcpy(response, "Result: ");
                    fmt_double(response + strlen(response), r);
                }
            }
        }
    }
    else if (!strcmp(cmd, "ping")) {
        if (nargs < 2) strcpy(response, "Usage: ping <hostname>");
        else { typed = 0; start_ping(t, args[1]); }
    }
    else if (!strcmp(cmd, "sysinfo")) {
        char a[32], b[32];
        struct rtc_time rt;
        rtc_read(&rt);
        fmt_size(a, vfs_usage_bytes());
        fmt_pad2(b, rt.hour);
        strcpy(response, "SCos System Information:\n");
        strcat(response, "OS Version: 2.0.0\n");
        strcat(response, "Kernel: SCos-2.0.0-native\n");
        strcat(response, "Architecture: x86 (32-bit protected mode)\n");
        strcat(response, "Uptime: "); fmt_u32(b, uptime_ms() / 60000); strcat(response, b); strcat(response, " minutes\n");
        strcat(response, "Total Memory: "); fmt_u32(b, mm_total_kb() / 1024); strcat(response, b); strcat(response, " MB\n");
        strcat(response, "Free Memory: "); fmt_u32(b, mm_free_kb() / 1024); strcat(response, b); strcat(response, " MB\n");
        strcat(response, "CPU Cores: 1\n");
        strcat(response, "Storage Used: "); strcat(response, a); strcat(response, "\n");
        strcat(response, "Disk: "); strcat(response, ata_present() ? "ATA present" : "none");
        strcat(response, fs_image_found ? " (SCos image loaded)" : "");
        strcat(response, "\nCurrent Time: ");
        fmt_pad2(b, rt.hour); strcat(response, b); strcat(response, ":");
        fmt_pad2(b, rt.min); strcat(response, b); strcat(response, ":");
        fmt_pad2(b, rt.sec); strcat(response, b);
    }
    else if (!strcmp(cmd, "alias")) {
        if (nargs == 1) {
            if (!t->nalias) strcpy(response, "No aliases set.");
            else {
                strcpy(response, "Current aliases:\n");
                for (int a2 = 0; a2 < t->nalias; a2++) {
                    strcat(response, t->aliases[a2][0]);
                    strcat(response, "='");
                    strcat(response, t->aliases[a2][1]);
                    strcat(response, "'\n");
                }
            }
        } else if (nargs >= 3 && t->nalias < 8) {
            strcpy(t->aliases[t->nalias][0], args[1]);
            strcpy(t->aliases[t->nalias][1], args[2]);
            t->nalias++;
            strcpy(response, "Alias '"); strcat(response, args[1]);
            strcat(response, "' set to '"); strcat(response, args[2]); strcat(response, "'");
        } else strcpy(response, "Usage: alias <name> '<command>'");
    }
    else if (!strcmp(cmd, "history")) {
        if (!t->hcount) strcpy(response, "No commands in history.");
        else {
            for (int j = 0; j < t->hcount; j++) {
                char num[8];
                fmt_u32(num, j + 1);
                strcat(response, num); strcat(response, "  ");
                strcat(response, t->hist[j]);
                strcat(response, "\n");
            }
        }
    }
    else if (!strcmp(cmd, "open")) {
        if (nargs < 2) strcpy(response, "Usage: open <app-id>  (files, terminal, notepad, browser, calendar, settings, about)");
        else if (app_find(args[1]) && args[1][0] != '_') {
            wm_open_app(args[1], NULL);
            strcpy(response, "Launched "); strcat(response, args[1]);
        } else { strcpy(response, "Unknown app: "); strcat(response, args[1]); }
    }
    else if (!strcmp(cmd, "save")) {
        if (!ata_present()) strcpy(response, "No ATA disk detected.");
        else if (fs_image_save()) strcpy(response, "Filesystem image written to disk.");
        else strcpy(response, "Error: disk write failed.");
    }
    else if (!strcmp(cmd, "shutdown")) {
        term_print(t, "Shutting down SCos... Goodbye!");
        t->shutting_down = 1;
        return;
    }
    else if (!strcmp(cmd, "reboot")) {
        term_print(t, "Rebooting SCos... Please wait.");
        t->shutting_down = 2;
        return;
    }
    else {
        strcpy(response, "Command not found: "); strcat(response, cmd);
        strcat(response, ". Type 'help' for available commands.");
    }

    if (response[0]) {
        if (typed) term_type(t, response);
        else term_print(t, response);
    }
}

/* ------------------------------------------------------------- app glue -- */
static void term_open(struct window *w, void *arg)
{
    (void)arg;
    struct term *t = palloc(sizeof(struct term));
    memset(t, 0, sizeof(*t));
    strcpy(t->cwd, "/home/");
    t->scroll = -1;
    t->hindex = 0;
    w->data = t;
    term_print(t, "SCos Terminal v2.0 (native kernel)");
    term_print(t, "Type 'help' for available commands.");
}

static void term_close(struct window *w)
{
    if (w->data) pfree(w->data, sizeof(struct term));
    w->data = NULL;
}

static int term_visible_rows(struct window *w)
{
    return (wm_content_h(w) - 24) / (FONT_H + 2);
}

static void term_paint(struct window *w)
{
    struct term *t = w->data;
    struct surface *s = &w->surf;
    const struct theme *th = theme_current();
    s_fill(s, 0, 0, s->w, s->h, 0x000000);

    int rows = term_visible_rows(w);
    /* pending typewriter text may span several lines */
    char pbuf[512];
    int plines = 0;
    if (t->pending_active) {
        int n = t->pend_shown;
        if (n > t->pend_len) n = t->pend_len;
        if (n > (int)sizeof(pbuf) - 1) n = sizeof(pbuf) - 1;
        memcpy(pbuf, t->pending, n);
        pbuf[n] = 0;
        plines = 1;
        for (int i = 0; i < n; i++) if (pbuf[i] == '\n') plines++;
    }
    int total = t->nlines + plines + 1;
    if (t->scroll < 0 || t->scroll > total - rows) t->scroll = total - rows;
    if (t->scroll < 0) t->scroll = 0;

    int y = 4;
    for (int r = t->scroll; r < total && r < t->scroll + rows; r++, y += FONT_H + 2) {
        if (r < t->nlines) {
            s_clip_text(s, 6, y, t->lines[r], th->main, s->w - 12);
        } else if (t->pending_active && r < t->nlines + plines) {
            const char *l = pbuf;
            for (int k = 0; k < r - t->nlines; k++) {
                l = str_chr(l, '\n');
                if (!l) break;
                l++;
            }
            char one[160];
            const char *nl = str_chr(l, '\n');
            int ln = nl ? (int)(nl - l) : (int)strlen(l);
            if (ln > (int)sizeof(one) - 1) ln = sizeof(one) - 1;
            memcpy(one, l, ln);
            one[ln] = 0;
            s_clip_text(s, 6, y, one, th->main, s->w - 12);
        } else {
            char prompt[160];
            prompt_str(t, prompt);
            s_text(s, 6, y, prompt, th->main);
            int px = 6 + s_text_width(prompt);
            s_clip_text(s, px, y, t->input, th->main, s->w - px - 8);
            if ((tick_count / 50) % 2 == 0)
                s_fill(s, px + t->ipos * FONT_W, y, 2, FONT_H, th->main);
        }
    }
}

static void term_key(struct window *w, struct key_event *e)
{
    struct term *t = w->data;
    if (!e->pressed) return;
    if (t->pending_active || t->ping_active) return;

    if (e->keycode == '\n') {
        char cmd[TERM_LINE];
        strcpy(cmd, t->input);
        t->input[0] = 0;
        t->ipos = 0;
        run_command(t, cmd);
        return;
    }
    if (e->keycode == KEY_UP) {
        if (t->hcount && t->hindex > 0) {
            t->hindex--;
            strcpy(t->input, t->hist[t->hindex]);
            t->ipos = (int)strlen(t->input);
        }
        return;
    }
    if (e->keycode == KEY_DOWN) {
        if (t->hindex < t->hcount - 1) {
            t->hindex++;
            strcpy(t->input, t->hist[t->hindex]);
        } else { t->hindex = t->hcount; t->input[0] = 0; }
        t->ipos = (int)strlen(t->input);
        return;
    }
    edit_line(t->input, &t->ipos, sizeof(t->input), e);
}

static void term_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct term *t = w->data;
    (void)x; (void)y;
    if (e->type == MEV_WHEEL) {
        int rows = term_visible_rows(w);
        t->scroll -= e->wheel;
        if (t->scroll < 0) t->scroll = 0;
        int total = t->nlines + (t->pending_active ? 1 : 0) + 1;
        if (t->scroll > total - rows) t->scroll = total - rows;
        if (t->scroll < 0) t->scroll = 0;
    }
}

static void term_tick(struct window *w)
{
    struct term *t = w->data;
    if (t->pending_active) {
        t->pend_shown += 10;                     /* typewriter speed */
        if (t->pend_shown >= t->pend_len) {
            term_push_line(t, t->pending);
            t->pending_active = 0;
        }
        wm_redraw(w);
    }
    if (t->ping_active) {
        if (tick_count >= t->ping_next) {
            u32 len = 0;
            char *json = vfs_read("system/network.json", &len);
            int online = json && str_str(json, "\"online\"") &&
                         str_str(json, t->ping_host);
            /* crude: look up the host block then its status */
            char blk[256];
            online = 0;
            char lat[24] = "45ms";
            char ip[24] = "0.0.0.0";
            if (json) {
                char *h = str_str(json, t->ping_host);
                if (h) {
                    strncpy(blk, h, sizeof(blk) - 1);
                    blk[sizeof(blk) - 1] = 0;
                    char *st = str_str(blk, "\"status\": \"");
                    if (st) {
                        st += 11;
                        online = !strncmp(st, "online", 6);
                    }
                    char *la = str_str(blk, "\"latency\": \"");
                    if (la) {
                        la += 12;
                        int i = 0;
                        while (la[i] && la[i] != '"' && i < 23) { lat[i] = la[i]; i++; }
                        lat[i] = 0;
                    }
                    char *ipx = str_str(blk, "\"ip\": \"");
                    if (ipx) {
                        ipx += 7;
                        int i = 0;
                        while (ipx[i] && ipx[i] != '"' && i < 23) { ip[i] = ipx[i]; i++; }
                        ip[i] = 0;
                    }
                }
            }
            char line[160];
            if (t->ping_seq < 4) {
                if (online) {
                    strcpy(line, "64 bytes from "); strcat(line, ip);
                    strcat(line, ": icmp_seq=");
                    char n[8]; fmt_u32(n, t->ping_seq + 1); strcat(line, n);
                    strcat(line, " ttl=64 time="); strcat(line, lat);
                    t->ping_next = tick_count + 15;
                } else {
                    strcpy(line, "Request timeout for icmp_seq ");
                    char n[8]; fmt_u32(n, t->ping_seq); strcat(line, n);
                    t->ping_next = tick_count + 100;
                }
                term_print(t, line);
                t->ping_seq++;
            } else {
                strcpy(line, "--- "); strcat(line, t->ping_host);
                strcat(line, " ping statistics ---\n");
                if (online) {
                    strcat(line, "4 packets transmitted, 4 received, 0% packet loss\n");
                    strcat(line, "round-trip min/avg/max = "); strcat(line, lat);
                } else {
                    strcat(line, "4 packets transmitted, 0 received, 100% packet loss, time 3000ms");
                }
                term_print(t, line);
                t->ping_active = 0;
            }
            wm_redraw(w);
        }
    }
    if (t->shutting_down == 1) {
        sleep_ms(1200);
        if (acpi_shutdown()) { /* may return on machines without ACPI */ }
        wm_poweroff_screen();
        for (;;) cpu_hlt();
    }
    if (t->shutting_down == 2) {
        sleep_ms(1200);
        cpu_reboot_8042();
    }
}

struct app app_terminal = {
    .id = "terminal", .title = "Terminal", .icon = ICON_TERMINAL, .single = 0,
    .def_w = 700, .def_h = 450,
    .open = term_open, .paint = term_paint, .key = term_key,
    .mouse = term_mouse, .tick = term_tick, .close = term_close,
};
