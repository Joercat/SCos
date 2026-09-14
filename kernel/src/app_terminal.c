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
    int scroll;              /* first visible line */
    int follow;              /* stick to bottom while new output arrives */
    char input[TERM_LINE];
    int ipos;
    char hist[HIST][TERM_LINE];
    int hcount, hindex;
    char cwd[128];
    /* typewriter effect */
    char pending[2048];
    int pend_len, pend_shown, pending_active;
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
    if (t->follow) t->scroll = 1000000;      /* stay glued to the new output */
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
    "help      - Show this help message\n"
    "ls        - List directory contents\n"
    "cd        - Change directory\n"
    "pwd       - Print working directory\n"
    "cat       - Show file contents\n"
    "head      - Show first lines of a file\n"
    "wc        - Count lines, words, bytes of a file\n"
    "grep      - Print lines matching a string\n"
    "hexdump   - Dump file bytes in hex\n"
    "tree      - Recursive directory tree\n"
    "echo      - Display a message\n"
    "clear     - Clear terminal screen\n"
    "date      - Show current date and time (CMOS clock)\n"
    "cal       - Show this month's calendar\n"
    "mkdir     - Create directory\n"
    "touch     - Create an empty file\n"
    "cp        - Copy a file\n"
    "mv        - Move or rename a file\n"
    "rm        - Delete file or directory\n"
    "whoami    - Show current user\n"
    "version   - Show system version\n"
    "uptime    - Time since boot (PIT)\n"
    "free      - Memory usage (real page allocator)\n"
    "cpu       - CPU brand from CPUID\n"
    "df        - Filesystem usage\n"
    "disks     - Detected ATA disks (IDENTIFY)\n"
    "neofetch  - System summary with logo\n"
    "sysmon    - Open the System Monitor app\n"
    "theme     - List or switch themes\n"
    "calc      - Perform basic arithmetic\n"
    "ping      - Check host reachability\n"
    "sysinfo   - Display system information\n"
    "alias     - Create command aliases\n"
    "history   - Show command history\n"
    "edit      - Open a file in Notepad\n"
    "open      - Launch a desktop app\n"
    "blackjack - Play blackjack\n"
    "save      - Write the filesystem image to disk\n"
    "shutdown  - Power the machine off\n"
    "reboot    - Restart the machine\n";

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

    if (!strcmp(cmd, "help")) {
        int page = 1;
        if (nargs > 1 && !strncmp(args[1], "--p", 3)) page = (int)str_to_u32(args[1] + 3);
        else if (nargs > 1) page = (int)str_to_u32(args[1]);
        if (page < 1) page = 1;
        const int PER = 18;
        int total_lines = 1;
        for (const char *q = help_text; *q; q++) if (*q == '\n') total_lines++;
        int pages = (total_lines + PER - 1) / PER;
        if (page > pages) page = pages;
        response[0] = 0;
        int line = 0;
        for (const char *q = help_text; *q; ) {
            const char *nl = str_chr(q, '\n');
            int len = nl ? (int)(nl - q) : (int)strlen(q);
            if (line >= (page - 1) * PER && line < page * PER) {
                strncat(response, q, (u32)len);
                strcat(response, "\n");
            }
            line++;
            if (!nl) break;
            q = nl + 1;
        }
        char foot[64];
        strcpy(foot, "-- page ");
        char n[8];
        fmt_u32(n, (u32)page); strcat(foot, n); strcat(foot, "/");
        fmt_u32(n, (u32)pages); strcat(foot, n);
        if (page < pages) { strcat(foot, " : 'help --p");
            fmt_u32(n, (u32)(page + 1)); strcat(foot, n); strcat(foot, "' for more --"); }
        else strcat(foot, " --");
        strcat(response, foot);
    }
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
    t->follow = 1;
        t->follow = 1;
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
        strcpy(response, "ping: this kernel has no TCP/IP stack - network unreachable.\n"
                         "No network interface driver is present (see 'disks'/'cpu' for real hardware).");
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
    else if (!strcmp(cmd, "pwd")) {
        strcpy(response, t->cwd);
    }
    else if (!strcmp(cmd, "cp")) {
        if (nargs < 3) strcpy(response, "Usage: cp <src> <dst>");
        else {
            char a[256], b[256];
            resolve_path(t, args[1], a); resolve_path(t, args[2], b);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: cannot read source");
            else if (vfs_write(b, data, len)) { strcpy(response, "Copied to "); strcat(response, b); }
            else strcpy(response, "Error: cannot write destination");
        }
    }
    else if (!strcmp(cmd, "mv")) {
        if (nargs < 3) strcpy(response, "Usage: mv <src> <dst>");
        else {
            char a[256], b[256];
            resolve_path(t, args[1], a); resolve_path(t, args[2], b);
            if (vfs_rename(a, b)) { strcpy(response, "Moved to "); strcat(response, b); }
            else strcpy(response, "Error: rename failed (missing path or name taken)");
        }
    }
    else if (!strcmp(cmd, "head")) {
        if (nargs < 2) strcpy(response, "Usage: head <file> [lines]");
        else {
            char a[256];
            resolve_path(t, args[1], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                int n = nargs > 2 ? (int)str_to_u32(args[2]) : 10;
                if (n < 1) n = 1;
                response[0] = 0;
                const char *p = data;
                for (int i = 0; i < n && *p; i++) {
                    const char *nl = str_chr(p, '\n');
                    int l = nl ? (int)(nl - p) : (int)strlen(p);
                    if ((int)strlen(response) + l + 2 > (int)sizeof(response) - 1) break;
                    strncat(response, p, l);
                    strcat(response, "\n");
                    if (!nl) break;
                    p = nl + 1;
                }
            }
        }
    }
    else if (!strcmp(cmd, "wc")) {
        if (nargs < 2) strcpy(response, "Usage: wc <file>");
        else {
            char a[256];
            resolve_path(t, args[1], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                u32 lines = 0, words = 0;
                int inw = 0;
                for (u32 i = 0; i < len; i++) {
                    char c = data[i];
                    if (c == '\n') lines++;
                    if (c == ' ' || c == '\n' || c == '\t') inw = 0;
                    else if (!inw) { inw = 1; words++; }
                }
                char n[12];
                response[0] = 0;
                fmt_u32(n, lines); strcat(response, n); strcat(response, " lines, ");
                fmt_u32(n, words); strcat(response, n); strcat(response, " words, ");
                fmt_u32(n, len); strcat(response, n); strcat(response, " bytes");
            }
        }
    }
    else if (!strcmp(cmd, "grep")) {
        if (nargs < 3) strcpy(response, "Usage: grep <text> <file>");
        else {
            char a[256];
            resolve_path(t, args[2], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                response[0] = 0;
                const char *p = data;
                while (*p) {
                    const char *nl = str_chr(p, '\n');
                    int l = nl ? (int)(nl - p) : (int)strlen(p);
                    char one[256];
                    if (l > (int)sizeof(one) - 1) l = sizeof(one) - 1;
                    memcpy(one, p, l);
                    one[l] = 0;
                    if (str_str(one, args[1])) {
                        if ((int)strlen(response) + l + 2 < (int)sizeof(response) - 1) {
                            strcat(response, one); strcat(response, "\n");
                        }
                    }
                    if (!nl) break;
                    p = nl + 1;
                }
                if (!response[0]) strcpy(response, "(no matches)");
            }
        }
    }
    else if (!strcmp(cmd, "hexdump")) {
        if (nargs < 2) strcpy(response, "Usage: hexdump <file>");
        else {
            char a[256];
            resolve_path(t, args[1], a);
            u32 len = 0;
            char *data = vfs_read(a, &len);
            if (!data) strcpy(response, "Error: File not found");
            else {
                if (len > 128) len = 128;
                response[0] = 0;
                static const char hx[] = "0123456789abcdef";
                for (u32 i = 0; i < len; i += 16) {
                    char line[80];
                    int o = 0;
                    for (u32 j = i; j < i + 16 && j < len; j++) {
                        line[o++] = hx[(u8)data[j] >> 4];
                        line[o++] = hx[data[j] & 15];
                        line[o++] = ' ';
                    }
                    line[o++] = '|';
                    for (u32 j = i; j < i + 16 && j < len; j++)
                        line[o++] = (data[j] >= 32 && data[j] < 127) ? data[j] : '.';
                    line[o++] = '|'; line[o] = 0;
                    if ((int)strlen(response) + o + 2 < (int)sizeof(response) - 1) {
                        strcat(response, line); strcat(response, "\n");
                    }
                }
            }
        }
    }
    else if (!strcmp(cmd, "tree")) {
        char path[256];
        resolve_path(t, nargs > 1 ? args[1] : "", path);
        struct vfs_node *n = vfs_lookup(path);
        if (!n || !vfs_is_dir(n)) strcpy(response, "Error: Invalid path");
        else {
            response[0] = 0;
            char names[64][VFS_NAME];
            /* iterative deep walk with indentation */
            struct { struct vfs_node *n; int depth; } stack[16];
            int sp = 0;
            stack[sp].n = n; stack[sp].depth = 0; sp++;
            while (sp && (int)strlen(response) < 1200) {
                sp--;
                struct vfs_node *cur = stack[sp].n;
                int d = stack[sp].depth;
                int c = vfs_list(cur, names, 64);
                for (int i = c - 1; i >= 0; i--) {
                    struct vfs_node *ch = vfs_child(cur, names[i]);
                    if (sp < 16 && ch && ch->is_dir) { stack[sp].n = ch; stack[sp].depth = d + 1; sp++; }
                }
                for (int i = 0; i < c; i++) {
                    for (int k = 0; k < d; k++) strcat(response, "  ");
                    strcat(response, names[i]);
                    struct vfs_node *ch = vfs_child(cur, names[i]);
                    if (ch && ch->is_dir) strcat(response, "/");
                    strcat(response, "\n");
                }
            }
        }
    }
    else if (!strcmp(cmd, "uptime")) {
        u32 up = uptime_ms() / 1000;
        char n[12];
        strcpy(response, "up ");
        fmt_u32(n, up / 3600); strcat(response, n); strcat(response, "h ");
        fmt_u32(n, (up / 60) % 60); strcat(response, n); strcat(response, "m ");
        fmt_u32(n, up % 60); strcat(response, n); strcat(response, "s");
    }
    else if (!strcmp(cmd, "free")) {
        u32 tot = 0, fre = 0;
        mm_stats(&tot, &fre);
        char n[16];
        strcpy(response, "total: ");
        fmt_u32(n, tot); strcat(response, n); strcat(response, " KB\nfree:  ");
        fmt_u32(n, fre); strcat(response, n); strcat(response, " KB\nused:  ");
        fmt_u32(n, tot - fre); strcat(response, n); strcat(response, " KB");
    }
    else if (!strcmp(cmd, "cpu")) {
        char cpu[49];
        cpu_brand(cpu, sizeof(cpu));
        strcpy(response, cpu[0] ? cpu : "x86 processor (no CPUID brand string)");
        char n[16];
        strcat(response, "\nspeed:   ");
        fmt_u32(n, cpu_mhz()); strcat(response, n); strcat(response, " MHz (measured via TSC/PIT)");
        strcat(response, "\nload:    ");
        fmt_u32(n, cpu_usage_pct()); strcat(response, n); strcat(response, "% (measured idle time)");
        strcat(response, "\nmode:    32-bit protected mode, ring 0, PIT @ 100 Hz");
    }
    else if (!strcmp(cmd, "df")) {
        char n[16];
        strcpy(response, "vfs:   ");
        fmt_u32(n, vfs_usage_bytes()); strcat(response, n); strcat(response, " bytes used (in memory)\n");
        strcat(response, "disk:  ");
        if (ata_present()) { strcat(response, "fs image at LBA 2048, 'save' writes, loaded at boot"); }
        else strcat(response, "no ATA disk");
    }
    else if (!strcmp(cmd, "disks")) {
        const char *m = ata_model();
        if (!m) strcpy(response, "No ATA disks detected on primary bus.");
        else { strcpy(response, "ATA0:  "); strcat(response, m); }
    }
    else if (!strcmp(cmd, "theme")) {
        if (nargs < 2) {
            strcpy(response, "Current theme: ");
            strcat(response, theme_current()->name);
            strcat(response, "\nAvailable:");
            for (int i = 0; i < theme_count(); i++) {
                strcat(response, "\n  "); strcat(response, theme_get(i)->id);
                strcat(response, "  - "); strcat(response, theme_get(i)->name);
            }
            strcat(response, "\nUsage: theme <id>");
        } else {
            int found = -1;
            for (int i = 0; i < theme_count(); i++)
                if (!strcmp(theme_get(i)->id, args[1])) found = i;
            if (found < 0) { strcpy(response, "Unknown theme: "); strcat(response, args[1]); }
            else { theme_set_index(found); settings_save();
                   strcpy(response, "Theme switched to "); strcat(response, theme_current()->name); }
        }
    }
    else if (!strcmp(cmd, "cal")) {
        struct rtc_time rt;
        rtc_read(&rt);
        response[0] = 0;
        static const char *mn[] = { "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December" };
        strcat(response, mn[rt.mon - 1]); strcat(response, " ");
        char n[8];
        fmt_u32(n, (u32)rt.year); strcat(response, n); strcat(response, "\n");
        strcat(response, "Su Mo Tu We Th Fr Sa\n");
        static const u8 md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int days = md[rt.mon - 1];
        if (rt.mon == 2 && ((rt.year % 4 == 0 && rt.year % 100 != 0) || rt.year % 400 == 0)) days = 29;
        /* weekday of day 1 from rtc weekday of today */
        int wd1 = (rt.weekday - (rt.day - 1) % 7 + 7) % 7;
        for (int i = 0; i < wd1; i++) strcat(response, "   ");
        for (int d = 1; d <= days; d++) {
            char cell[8];
            if (d == rt.day) { cell[0] = '['; fmt_u32(cell + 1, (u32)d);
                strcat(cell, "]"); }
            else { fmt_u32(cell, (u32)d); if (d < 10) { cell[1] = cell[0]; cell[0] = ' '; cell[2] = 0; } }
            strcat(response, cell);
            strcat(response, (d == rt.day) ? "" : " ");
            if ((wd1 + d) % 7 == 0) strcat(response, "\n");
        }
    }
    else if (!strcmp(cmd, "neofetch")) {
        char cpu[49];
        cpu_brand(cpu, sizeof(cpu));
        if (!cpu[0]) strcpy(cpu, "unknown x86 processor");
        u32 tot = 0, fre = 0;
        mm_stats(&tot, &fre);
        char n[16];
        char info[14][64];
        int ni = 0;
        strcpy(info[ni++], "user@scos");
        strcpy(info[ni++], "---------------------");
        strcpy(info[ni++], "OS:      SCos 2.0.0 (native x86 kernel)");
        strcpy(info[ni],   "CPU:     "); strncpy(info[ni] + 9, cpu, 40); ni++;
        strcpy(info[ni],   "Speed:   ");
        fmt_u32(n, cpu_mhz()); strcat(info[ni], n); strcat(info[ni], " MHz (TSC-measured)"); ni++;
        strcpy(info[ni],   "Load:    ");
        fmt_u32(n, cpu_usage_pct()); strcat(info[ni], n); strcat(info[ni], "% (idle-time meter)"); ni++;
        strcpy(info[ni],   "Memory:  ");
        fmt_u32(n, (tot - fre) / 1024); strcat(info[ni], n);
        strcat(info[ni], " / "); fmt_u32(n, tot / 1024); strcat(info[ni], n);
        strcat(info[ni], " MB"); ni++;
        strcpy(info[ni],   "Disk:    ");
        { const char *m = ata_model();
          strncpy(info[ni] + 9, m && m[0] ? m : "none", 40); }
        ni++;
        strcpy(info[ni],   "Video:   ");
        fmt_u32(n, (u32)screen_w); strcat(info[ni], n); strcat(info[ni], "x");
        fmt_u32(n, (u32)screen_h); strcat(info[ni], n); strcat(info[ni], "x");
        fmt_u32(n, fb_bpp()); strcat(info[ni], n); ni++;
        strcpy(info[ni],   "Uptime:  ");
        fmt_u32(n, uptime_ms() / 1000); strcat(info[ni], n); strcat(info[ni], " s"); ni++;
        strcpy(info[ni],   "Theme:   ");
        strncpy(info[ni] + 9, theme_current()->name, 40); ni++;
        strcpy(info[ni++], "Shell:   scos-sh");
        strcpy(info[ni++], "WM:      SCos WM (VBE framebuffer)");
        response[0] = 0;
        int rows = neofetch_art_lines > ni ? neofetch_art_lines : ni;
        for (int i = 0; i < rows; i++) {
            char line[128];
            const char *a = i < neofetch_art_lines ? neofetch_art[i] : "";
            strcpy(line, a);
            int l = (int)strlen(line);
            while (l < neofetch_art_width + 2) line[l++] = ' ';
            line[l] = 0;
            if (i < ni) strcat(line, info[i]);
            strcat(response, line);
            strcat(response, "\n");
        }
        response[strlen(response) - 1] = 0;
    }
    else if (!strcmp(cmd, "edit")) {
        if (nargs < 2) strcpy(response, "Usage: edit <file>");
        else {
            char a[256];
            resolve_path(t, args[1], a);
            wm_open_app("notepad", a);
            strcpy(response, "Opened in Notepad: "); strcat(response, a);
        }
    }
    else if (!strcmp(cmd, "blackjack")) {
        wm_open_app("blackjack", NULL);
        strcpy(response, "Blackjack table opened.");
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
    t->follow = (t->scroll >= total - rows);

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
    /* never swallow keystrokes: finish any typewriter output instantly */
    if (t->pending_active) {
        term_push_line(t, t->pending);
        t->pending_active = 0;
    }

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
        t->follow = (t->scroll >= total - rows);
    }
}

static void term_tick(struct window *w)
{
    struct term *t = w->data;
    if (t->pending_active) {
        t->pend_shown += 40;                     /* typewriter speed */
        if (t->pend_shown >= t->pend_len) {
            term_push_line(t, t->pending);
            t->pending_active = 0;
        }
        wm_redraw(w);
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
