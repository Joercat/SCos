/* SCos secure screen: sign-in, first-run setup, session lock, masked password
 * prompts and recovery.  These flows own the framebuffer directly rather than
 * going through a window, so no compositor widget, window surface or terminal
 * scrollback ever holds a typed secret.  The only characters ever painted for
 * a masked field are '*'.  Every input buffer is scrubbed on the way out.
 *
 * The screen is deliberately driven by the same kbd_poll/usb_poll path as the
 * WM (no second input stack), and calls cpu_hlt() so the machine stays idle
 * while waiting for a person.
 */
#include "scos.h"
#include "security.h"

#define UI_FIELDS 3
#define UI_BG     0x00070d16u
#define UI_WARN   0x00ffbb55u
#define UI_ERROR  0x00ff7070u
#define UI_DIM    0x008090a0u
#define UI_FIELD  0x00101c2au

struct ui_field {
    const char *label;
    char  buf[SC_PASS_MAX + 1];
    int   pos, max, visible;
};

struct ui_screen {
    const char *title, *subtitle, *hint, *footer;
    int nfields, active;
    int check_gate, allow_cancel, allow_recovery, allow_switch, show_help;
    int caps;
    char msg[SC_MSG_MAX];
    struct ui_field f[UI_FIELDS];
};

typedef int (*ui_submit)(struct ui_screen *s, void *ctx);

/* ------------------------------------------------------------- input/plumbing --- */
static void drain_input(void)
{
    struct key_event ke;
    struct mouse_event me;
    for (int i = 0; i < 256 && kbd_poll(&ke); i++) { }
    for (int i = 0; i < 256 && mouse_poll(&me); i++) { }
}

static void wipe_screen(void)
{
    fb_scene_unclip();
    fb_clear(UI_BG);
    fb_flip();
}

static void screen_init(struct ui_screen *s, const char *title, const char *subtitle)
{
    memset(s, 0, sizeof(*s));
    s->title = title;
    s->subtitle = subtitle;
    s->hint = "Press F1 to show the current password policy.";
    s->footer = "[Enter] continue    [Esc] cancel    [F1] policy";
    s->show_help = 0;
    for (int i = 0; i < UI_FIELDS; i++) {
        s->f[i].label = "";
        s->f[i].max = SC_PASS_MAX + 1;
    }
}

static void field(struct ui_screen *s, int i, const char *label, int visible, int max)
{
    if (i < 0 || i >= UI_FIELDS) return;
    s->f[i].label = label;
    s->f[i].visible = visible;
    s->f[i].max = max;
    s->f[i].buf[0] = 0;
    s->f[i].pos = 0;
    if (i + 1 > s->nfields) s->nfields = i + 1;
}

static void scrub(struct ui_screen *s)
{
    for (int i = 0; i < UI_FIELDS; i++) sc_burn(s->f[i].buf, sizeof(s->f[i].buf));
}

static const char *field_display(const struct ui_field *f, char *buf, size_t cap)
{
    size_t n;
    if (f->visible) return f->buf;
    n = sc_len(f->buf);
    if (n > 40) n = 40;
    for (size_t i = 0; i < n && i + 1 < cap; i++) buf[i] = '*';
    buf[n] = 0;
    if (sc_len(f->buf) > 40 && n + 1 < cap) buf[n++] = '+';
    if (n < cap) buf[n] = 0;
    return buf;
}

/* ------------------------------------------------------------------ painting --- */
static void status_line(char *out, size_t cap)
{
    struct rtc_time r;
    char n[16];
    rtc_read(&r);
    sc_copy(out, cap, "SCos " SCOS_BUILD_TAG "  |  ");
    sc_fmt(n, sizeof(n), r.year); sc_cat(out, cap, n); sc_cat(out, cap, "-");
    fmt_pad2(n, r.mon); sc_cat(out, cap, n); sc_cat(out, cap, "-");
    fmt_pad2(n, r.day); sc_cat(out, cap, n); sc_cat(out, cap, " ");
    fmt_pad2(n, r.hour); sc_cat(out, cap, n); sc_cat(out, cap, ":");
    fmt_pad2(n, r.min); sc_cat(out, cap, n);
    sc_cat(out, cap, "  |  ");
    sc_cat(out, cap, security_api_persistent() ? "disk-backed accounts" : "RAM-only accounts");
}

static void paint(struct ui_screen *s)
{
    const struct theme *t = theme_current();
    int W = screen_w, H = screen_h;
    int pw = W - 40 < 560 ? W - 40 : 560;
    int ph = s->nfields ? 150 + s->nfields * 48 + 78 : 320;
    int px, py, y;
    char status[192], disp[SC_PASS_MAX + 2];
    if (ph > H - 16) ph = H - 16;
    if (ph < 200) ph = 200;
    px = (W - pw) / 2;
    py = (H - ph) / 2;
    fb_scene_unclip();
    s_vgrad(&screen, 0, 0, W, H, t->bg_top, t->bg_bot);
    s_fill(&screen, px, py, pw, ph, t->win_bg);
    s_frame_rect(&screen, px, py, pw, ph, t->main);
    s_fill(&screen, px + 1, py + 1, pw - 2, 4, t->main);
    s_text_scaled(&screen, px + 18, py + 16, s->title, t->text, 2);
    if (s->subtitle) s_text(&screen, px + 18, py + 54, s->subtitle, t->main);
    status_line(status, sizeof(status));
    s_clip_text(&screen, px + 18, py + 74, status, UI_DIM, pw - 36);

    for (int i = 0; i < s->nfields; i++) {
        int y0 = py + 98 + i * 48;
        int by = y0 + 18;
        s_text(&screen, px + 18, y0, s->f[i].label, t->text);
        s_fill(&screen, px + 18, by, pw - 36, 26, UI_FIELD);
        s_frame_rect(&screen, px + 18, by, pw - 36, 26,
                     i == s->active ? t->main : UI_DIM);
        s_clip_text(&screen, px + 26, by + 5,
                    field_display(&s->f[i], disp, sizeof(disp)), t->text, pw - 52);
        if (i == s->active && (u32)(tick_count / 50) % 2 == 0) {
            int col = s->f[i].visible ? (int)sc_len(s->f[i].buf) :
                      (int)(sc_len(s->f[i].buf) > 40 ? 41 : sc_len(s->f[i].buf));
            if (col > (pw - 60) / 8) col = (pw - 60) / 8;
            s_fill(&screen, px + 26 + col * 8, by + 5, 2, 16, t->main);
        }
        if (!s->f[i].visible && s->caps && i == s->active)
            s_text(&screen, px + pw - 120, y0, "[CAPS]", UI_WARN);
    }

    y = py + ph - 92;
    if (!s->nfields && s->show_help && s->hint) {
        /* Full-page information (recovery code, verification result): give the
         * text the whole panel instead of the three-line hint strip. */
        s_text_wrap(&screen, px + 18, py + 96, pw - 36, s->hint, t->text);
    } else if (s->show_help && s->hint) {
        y += s_text_wrap(&screen, px + 18, y, pw - 36, s->hint, UI_DIM) * 18;
    }
    (void)y;
    if (security_api_denied()) {
        char wait[128], n[24];
        sc_fmt(n, sizeof(n), security_api_deny_remaining());
        sc_copy(wait, sizeof(wait), "Too many attempts. Wait ");
        sc_cat(wait, sizeof(wait), n); sc_cat(wait, sizeof(wait), " seconds.");
        s_text_wrap(&screen, px + 18, py + ph - 60, pw - 36, wait, UI_WARN);
    } else if (s->msg[0]) {
        s_text_wrap(&screen, px + 18, py + ph - 60, pw - 36, s->msg, UI_ERROR);
    }
    s_clip_text(&screen, px + 18, py + ph - 28, s->footer ? s->footer : "",
                t->text, pw - 36);
}

static int ui_run(struct ui_screen *s, ui_submit submit, void *ctx)
{
    int result = 0, dirty = 1;
    uint64_t last_blink = 0;
    drain_input();
    for (;;) {
        struct key_event ke;
        struct mouse_event me;
        if (s->check_gate && security_api_gate_satisfied()) { result = 1; break; }
        usb_poll();
        interrupt_poll();
        while (mouse_poll(&me)) { }             /* keep the queue empty, ignore clicks */
        while (kbd_poll(&ke)) {
            if (!ke.pressed) continue;
            security_mix_event(ke.scancode);
            if (ke.keycode == KEY_CAPS) { s->caps ^= 1; dirty = 1; continue; }
            if (ke.keycode == KEY_F1) { s->show_help ^= 1; dirty = 1; continue; }
            if (ke.keycode == KEY_F8 && s->allow_recovery) { result = -2; break; }
            if (s->allow_switch && ke.keycode == KEY_F2) { result = -3; break; }
            if (ke.keycode == 27 && s->allow_cancel) { result = 0; break; }
            if (ke.keycode == '\t') {
                if (s->nfields > 1) s->active = (s->active + 1) % s->nfields;
                dirty = 1;
                continue;
            }
            if (ke.keycode == '\n') {
                int r;
                if (security_api_denied()) { dirty = 1; continue; }
                r = submit ? submit(s, ctx) : 1;
                if (r == 1) { result = 1; break; }
                if (r == -1) { result = 0; break; }
                dirty = 1;
                continue;
            }
            if (s->nfields > 0 && s->active < s->nfields) {
                struct ui_field *f = &s->f[s->active];
                if (edit_line(f->buf, &f->pos, f->max, &ke)) dirty = 1;
            }
        }
        if (result) break;
        if (dirty || (u32)(tick_count / 50) != (u32)last_blink) {
            last_blink = tick_count / 50;
            paint(s);
            fb_flip();
            dirty = security_api_denied() ? 1 : 0;
        }
        cpu_hlt();
    }
    wipe_screen();
    scrub(s);
    drain_input();
    return result;
}

/* ------------------------------------------------------------------ submitters --- */
static int submit_ok(struct ui_screen *s, void *ctx) { (void)s; (void)ctx; return 1; }

static int submit_setup(struct ui_screen *s, void *ctx)
{
    int *submitted = (int *)ctx;
    if (!sc_eq(s->f[1].buf, s->f[2].buf)) {
        sc_copy(s->msg, sizeof(s->msg), "The two passwords do not match.");
        return 0;
    }
    if (!security_create_admin(s->f[0].buf, s->f[1].buf, s->f[2].buf)) {
        sc_copy(s->msg, sizeof(s->msg), security_api_last_error());
        return 0;
    }
    *submitted = 1;
    return 1;
}

static int submit_login(struct ui_screen *s, void *ctx)
{
    (void)ctx;
    if (!security_api_authenticate(s->f[0].buf, s->f[1].buf,
                                   s->msg, sizeof(s->msg))) {
        s->f[1].buf[0] = 0;
        s->f[1].pos = 0;
        s->active = 1;
        return 0;
    }
    return 1;
}

static int submit_unlock(struct ui_screen *s, void *ctx)
{
    (void)ctx;
    if (!security_api_verify(security_api_session(), s->f[0].buf,
                             s->msg, sizeof(s->msg))) {
        s->f[0].buf[0] = 0;
        s->f[0].pos = 0;
        return 0;
    }
    return 1;
}

struct secret_ctx { char *out; size_t cap; };

static int submit_secret(struct ui_screen *s, void *ctx)
{
    struct secret_ctx *c = (struct secret_ctx *)ctx;
    if (!sc_len(s->f[0].buf)) {
        sc_copy(s->msg, sizeof(s->msg), "Enter a password first.");
        return 0;
    }
    if (c && c->out && c->cap) sc_copy(c->out, c->cap, s->f[0].buf);
    return 1;
}

struct newpw_ctx { const char *user; char *out; size_t cap; };

static int submit_new_password(struct ui_screen *s, void *ctx)
{
    struct newpw_ctx *c = (struct newpw_ctx *)ctx;
    if (!sc_eq(s->f[0].buf, s->f[1].buf)) {
        sc_copy(s->msg, sizeof(s->msg), "The two passwords do not match.");
        return 0;
    }
    if (!security_api_password_ok(c->user, s->f[0].buf, s->msg, sizeof(s->msg)))
        return 0;
    sc_copy(c->out, c->cap, s->f[0].buf);
    return 1;
}

struct changepw_ctx { int idx; int require_old; };

static int submit_change_password(struct ui_screen *s, void *ctx)
{
    struct changepw_ctx *c = (struct changepw_ctx *)ctx;
    int off = c->require_old ? 1 : 0;
    const char *old = c->require_old ? s->f[0].buf : 0;
    if (!sc_eq(s->f[off].buf, s->f[off+1].buf)) {
        sc_copy(s->msg, sizeof(s->msg), "The two passwords do not match.");
        return 0;
    }
    if (!security_api_change_password(c->idx, old, s->f[off].buf,
                                      c->require_old, s->msg, sizeof(s->msg)))
        return 0;
    return 1;
}

static int submit_recovery(struct ui_screen *s, void *ctx)
{
    (void)ctx;
    if (!sc_eq(s->f[1].buf, s->f[2].buf)) {
        sc_copy(s->msg, sizeof(s->msg), "The two passwords do not match.");
        return 0;
    }
    if (!security_api_redeem_recovery(s->f[0].buf, s->f[1].buf,
                                      s->msg, sizeof(s->msg)))
        return 0;
    return 1;
}

/* --------------------------------------------------------------------- flows --- */
void security_ui_info(const char *title, const char *text)
{
    struct ui_screen s;
    screen_init(&s, title, 0);
    s.hint = text;
    s.nfields = 0;
    s.show_help = 1;
    s.footer = "[Enter] continue";
    ui_run(&s, submit_ok, 0);
}

int security_ui_secret(const char *title, const char *prompt, char *out, size_t cap)
{
    struct ui_screen s;
    struct secret_ctx c;
    int r;
    c.out = out; c.cap = cap;
    screen_init(&s, title, 0);
    field(&s, 0, prompt, 0, SC_PASS_MAX + 1);
    s.allow_cancel = 1;
    s.check_gate = 0;
    r = ui_run(&s, submit_secret, &c);
    return r == 1;
}

void security_ui_setup(void)
{
    char hint[192], text[320];
    struct ui_screen s;
    int submitted = 0, r;
    for (;;) {
        security_api_build_hint(hint, sizeof(hint));
        screen_init(&s, "Create the administrator", "First run - no accounts exist yet");
        field(&s, 0, "Username", 1, SC_NAME_MAX);
        field(&s, 1, "New password", 0, SC_PASS_MAX + 1);
        field(&s, 2, "Confirm password", 0, SC_PASS_MAX + 1);
        s.hint = hint;
        s.footer = "[Tab] next field    [Enter] create account    [F1] policy";
        s.show_help = 1;
        s.check_gate = 1;
        s.allow_cancel = 0;
        r = ui_run(&s, submit_setup, &submitted);
        (void)r;
        if (!security_api_gate_satisfied()) continue;
        if (submitted) {
            sc_copy(text, sizeof(text),
                    "Write this ONE-TIME recovery code down and keep it offline:\n\n    ");
            sc_cat(text, sizeof(text), security_api_recovery_code());
            sc_cat(text, sizeof(text),
                   "\n\nFrom the sign-in screen press F8 to use it to reset the "
                   "administrator password. It works once, is stored only as a "
                   "salted hash, and cannot be shown again. Press Enter when you "
                   "have saved it.");
            security_ui_info("Save your recovery code", text);
        }
        security_api_ack_recovery();
        return;
    }
}

void security_ui_login(void)
{
    char hint[192];
    struct ui_screen s;
    for (;;) {
        security_api_build_hint(hint, sizeof(hint));
        screen_init(&s, "SCos sign-in", "Authenticate to start the desktop");
        field(&s, 0, "Username", 1, SC_NAME_MAX);
        field(&s, 1, "Password", 0, SC_PASS_MAX + 1);
        s.hint = hint;
        s.footer = security_api_recovery_active() ?
            "[Tab] next field   [Enter] sign in   [F8] recovery   [F1] policy" :
            "[Tab] next field   [Enter] sign in   [F1] policy";
        s.check_gate = 1;
        s.allow_recovery = security_api_recovery_active();
        s.allow_cancel = 0;
        {
            int r = ui_run(&s, submit_login, 0);
            if (r == -2) {
                security_ui_recovery();
                if (security_api_gate_satisfied()) return;
                continue;
            }
        }
        if (security_api_gate_satisfied()) return;
        if (security_api_force_change()) {
            if (!security_ui_change_password(security_api_session(), 0))
                security_api_drop_session();
            if (security_api_gate_satisfied()) return;
        }
    }
}

void security_ui_logout(void)
{
    if (!security_api_user_count()) security_ui_setup();
    else security_ui_login();
}

void security_ui_lock(void)
{
    struct ui_screen s;
    if (security_api_session() < 0) { security_ui_logout(); return; }
    security_api_lock_session();
    for (;;) {
        char sub[128];
        sc_copy(sub, sizeof(sub), "Signed in as ");
        sc_cat(sub, sizeof(sub), security_api_session_name());
        screen_init(&s, "Session locked", sub);
        field(&s, 0, "Password", 0, SC_PASS_MAX + 1);
        s.footer = "[Enter] unlock    [F2] switch user    [F1] policy";
        s.allow_switch = 1;
        s.allow_cancel = 0;
        {
            int r = ui_run(&s, submit_unlock, 0);
            if (r == -3) {
                security_api_unlock_session();
                security_api_drop_session();
                security_ui_logout();
                return;
            }
            if (r == 1 || security_api_gate_satisfied()) break;
        }
    }
    security_api_unlock_session();
}

int security_ui_new_password(const char *title, const char *user, char *out, size_t cap)
{
    struct ui_screen s;
    struct newpw_ctx c;
    char hint[192];
    int r;
    security_api_build_hint(hint, sizeof(hint));
    c.user = user; c.out = out; c.cap = cap;
    screen_init(&s, title, user);
    field(&s, 0, "New password", 0, SC_PASS_MAX + 1);
    field(&s, 1, "Confirm password", 0, SC_PASS_MAX + 1);
    s.hint = hint;
    s.show_help = 1;
    s.allow_cancel = 1;
    s.footer = "[Tab] next field    [Enter] accept    [Esc] cancel    [F1] policy";
    r = ui_run(&s, submit_new_password, &c);
    return r == 1;
}

int security_ui_change_password(int idx, int require_old)
{
    struct ui_screen s;
    struct changepw_ctx c;
    char hint[192];
    int off = require_old ? 1 : 0, r;
    c.idx = idx; c.require_old = require_old;
    security_api_build_hint(hint, sizeof(hint));
    screen_init(&s, require_old ? "Change your password" : "Choose a new password",
                "A password change is required before continuing");
    if (require_old) field(&s, 0, "Current password", 0, SC_PASS_MAX + 1);
    field(&s, off, "New password", 0, SC_PASS_MAX + 1);
    field(&s, off + 1, "Confirm password", 0, SC_PASS_MAX + 1);
    s.nfields = off + 2;
    s.hint = hint;
    s.show_help = 1;
    s.allow_cancel = 1;
    s.footer = "[Tab] next field    [Enter] change    [Esc] cancel    [F1] policy";
    r = ui_run(&s, submit_change_password, &c);
    return r == 1;
}

int security_ui_reauth(const char *reason)
{
    char pw[SC_PASS_MAX + 1], msg[SC_MSG_MAX];
    for (int attempt = 0; attempt < 3; attempt++) {
        int ok;
        if (!security_ui_secret(reason, "Current password", pw, sizeof(pw))) {
            sc_burn(pw, sizeof(pw));
            return 0;
        }
        ok = security_api_verify(security_api_session(), pw, msg, sizeof(msg));
        sc_burn(pw, sizeof(pw));
        if (ok) return 1;
        security_ui_info("Verification failed", msg);
    }
    return 0;
}

int security_ui_user_add(const char *name, int role)
{
    char pw[SC_PASS_MAX + 1], title[128];
    int ok;
    sc_copy(title, sizeof(title), "Initial password for ");
    sc_cat(title, sizeof(title), name);
    ok = security_ui_new_password(title, name, pw, sizeof(pw));
    if (ok) ok = security_api_create_user(name, role, pw);
    sc_burn(pw, sizeof(pw));
    return ok;
}

int security_ui_user_del(const char *name)
{
    char reason[160];
    sc_copy(reason, sizeof(reason), "Delete account ");
    sc_cat(reason, sizeof(reason), name);
    sc_cat(reason, sizeof(reason), "? Enter your password.");
    if (!security_ui_reauth(reason)) return 0;
    return security_api_delete_user(name);
}

int security_ui_switch_user(const char *name)
{
    char prompt[160], pw[SC_PASS_MAX + 1], msg[SC_MSG_MAX];
    int ok;
    sc_copy(prompt, sizeof(prompt), "Password for ");
    sc_cat(prompt, sizeof(prompt), name);
    if (!security_ui_secret("Switch user", prompt, pw, sizeof(pw))) {
        sc_burn(pw, sizeof(pw));
        return 0;
    }
    ok = security_api_authenticate(name, pw, msg, sizeof(msg));
    sc_burn(pw, sizeof(pw));
    if (!ok) {
        security_ui_info("Sign-in failed", msg);
        return 0;
    }
    if (security_api_force_change() &&
        !security_ui_change_password(security_api_session(), 0)) {
        security_api_drop_session();
        return 0;
    }
    return security_api_gate_satisfied();
}

int security_ui_recovery(void)
{
    struct ui_screen s;
    int r;
    if (!security_api_recovery_active()) return 0;
    screen_init(&s, "Account recovery", "One-time code + a new administrator password");
    field(&s, 0, "Recovery code", 0, SC_PASS_MAX + 1);
    field(&s, 1, "New password", 0, SC_PASS_MAX + 1);
    field(&s, 2, "Confirm password", 0, SC_PASS_MAX + 1);
    s.hint = "The code was shown once when the account system was created or by "
             "'recovery' in Terminal. It is valid for one use only.";
    s.show_help = 1;
    s.allow_cancel = 1;
    s.footer = "[Tab] next field    [Enter] reset    [Esc] cancel";
    r = ui_run(&s, submit_recovery, 0);
    return r == 1;
}
