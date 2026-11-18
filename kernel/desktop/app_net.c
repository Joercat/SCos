/*
 * SCos native - Network window.
 *
 * The panel is the same data the terminal's `net' command prints and the taskbar's glyph summarises: one
 * call to net_get_info() for the state, and the probes underneath are the stack's own - a DHCP renew, an
 * ICMP echo to the gateway, and one plain HTTP GET.  Nothing is simulated here: a machine with no
 * supported card says so, a card with no cable says so, and a probe that fails reports the milliseconds it
 * waited before giving up rather than a green light.
 *
 * Three buttons, because those are the three things this stack can actually do: ask for a lease, ping, and
 * fetch a URL over TCP without TLS.  The last one is deliberately a fixed, readable endpoint: a text field
 * in a kernel with one TCP connection would be a second state machine, not a feature.
 */
#include "scos.h"
#include "net.h"

#define NET_FETCH_HOST "example.com"

struct netapp {
    int hover_btn;
    int pressed_btn;
    char result[512];           /* the last probe's own words, kept between paints */
    int  result_ok;
    int  result_kind;           /* 0 none, 1 renew, 2 ping, 3 fetch */
    u32  rtt_ms;
    u32  bytes;
};

/* One instance, so the state lives here rather than in a heap allocation per window. */
static struct netapp na_state;

static int button_rect(int index, int w, int *x, int *y)
{
    int bw = 108, gap = 10;
    int total = 3 * bw + 2 * gap;
    int left = (w - total) / 2;
    int top = 226;

    if (left < 12) { bw = (w - 24 - 2 * gap) / 3; if (bw < 48) bw = 48; left = 12; total = 3 * bw + 2 * gap; }
    *x = left + index * (bw + gap);
    *y = top;
    return bw;
}

static void na_paint(struct window *w)
{
    struct netapp *a = &na_state;
    struct surface *s = &w->surf;
    const struct theme *t = theme_current();
    struct net_info ni;
    char line[160], n[24], ip[20];

    net_get_info(&ni);
    s_fill(s, 0, 0, s->w, s->h, t->win_bg);

    int y = 12;
    s_text(s, 14, y, "Network", t->main);
    y += 24;

    /* Adapter.  The driver's own name is printed, because "rtl8139" and "rtl8168" are different register
     * files and a report that said "Ethernet" would hide which one answered. */
    strcpy(line, "Adapter:  ");
    if (ni.present) strcat(line, ni.driver);
    else strcat(line, "none - no supported PCI adapter");
    s_clip_text(s, 14, y, line, ni.present ? t->text : t->main, s->w - 28);
    y += 18;

    if (ni.present) {
        strcpy(line, "Address:  ");
        for (int i = 0; i < NET_ALEN; i++) {
            static const char hex[] = "0123456789abcdef";
            char two[4];
            two[0] = hex[(ni.mac[i] >> 4) & 15];
            two[1] = hex[ni.mac[i] & 15];
            two[2] = i == NET_ALEN - 1 ? 0 : ':';
            two[3] = 0;
            strcat(line, two);
        }
        s_clip_text(s, 14, y, line, t->text, s->w - 28);
        y += 18;

        strcpy(line, "Link:     ");
        if (ni.link) {
            fmt_u32(n, ni.speed_mbps);
            strcat(line, "up, ");
            strcat(line, n);
            strcat(line, " Mb/s");
        } else {
            strcat(line, "down");
        }
        s_clip_text(s, 14, y, line, ni.link ? t->text : t->main, s->w - 28);
        y += 18;

        strcpy(line, "State:    ");
        strcat(line, net_state_name(ni.state));
        strcat(line, " (lease ");
        strcat(line, net_lease_name(ni.lease));
        strcat(line, ")");
        s_clip_text(s, 14, y, line, t->text, s->w - 28);
        y += 18;

        net_format_ip(ip, sizeof ip, ni.ip);
        strcpy(line, "IP:       "); strcat(line, ip);
        net_format_ip(ip, sizeof ip, ni.mask);
        strcat(line, "  mask "); strcat(line, ip);
        s_clip_text(s, 14, y, line, t->text, s->w - 28);
        y += 18;
        net_format_ip(ip, sizeof ip, ni.gateway);
        strcpy(line, "Gateway:  "); strcat(line, ip);
        net_format_ip(ip, sizeof ip, ni.dns);
        strcat(line, "  DNS "); strcat(line, ip);
        s_clip_text(s, 14, y, line, t->text, s->w - 28);
        y += 18;

        strcpy(line, "Counters: ");
        fmt_u32(n, ni.rx_packets); strcat(line, n); strcat(line, " rx, ");
        fmt_u32(n, ni.tx_packets); strcat(line, n); strcat(line, " tx, ");
        fmt_u32(n, ni.dropped);    strcat(line, n); strcat(line, " dropped, ");
        fmt_u32(n, ni.arp_requests); strcat(line, n); strcat(line, " ARP, ");
        fmt_u32(n, ni.tcp_packets); strcat(line, n); strcat(line, " TCP");
        s_clip_text(s, 14, y, line, t->text, s->w - 28);
        y += 18;
    }

    /* Buttons. */
    static const char *labels[3] = { "Renew lease", "Ping gateway", "Fetch example.com" };
    for (int i = 0; i < 3; i++) {
        int bx, by, bw = button_rect(i, s->w, &bx, &by);
        int on = a->hover_btn == i;
        int enabled = ni.present && ni.link;
        u32 bg = !enabled ? color_blend(t->win_bg, t->main, 6)
                          : on || a->pressed_btn == i ? color_blend(t->win_bg, t->main, 40)
                          : color_blend(t->win_bg, t->main, 16);
        s_fill(s, bx, by, bw, 26, bg);
        s_frame_rect(s, bx, by, bw, 26, enabled ? t->main : color_blend(t->win_bg, t->main, 20));
        int tw = s_text_width(labels[i]);
        if (tw > bw - 8) tw = bw - 8;
        s_clip_text(s, bx + (bw - tw) / 2, by + 7, labels[i],
                    enabled ? t->text : color_blend(t->win_bg, t->text, 50), bw - 8);
    }

    /* The answer to whatever was last asked. */
    y = 268;
    if (a->result[0]) {
        s_clip_text(s, 14, y, a->result, a->result_ok ? t->main : color_blend(t->win_bg, t->main, 70), s->w - 28);
        y += 18;
        if (a->result_kind == 1 || a->result_kind == 3) {
            strcpy(line, "");
            fmt_u32(n, a->bytes); strcat(line, n); strcat(line, " byte(s)");
            s_clip_text(s, 14, y, line, t->text, s->w - 28);
        }
    }
    s_text(s, 14, s->h - 22, "DHCP, ICMP, DNS and plain HTTP/1.0; no TLS in this kernel.", t->text);
}

static void na_key(struct window *w, struct key_event *e)
{
    (void)w; (void)e;
}

static void na_run(struct window *w, int kind)
{
    struct netapp *a = &na_state;
    struct net_info ni;

    net_get_info(&ni);
    a->result_ok = 0;
    a->result_kind = kind;
    a->bytes = 0;
    if (!ni.present) { strcpy(a->result, "No supported adapter on this machine."); }
    else if (!ni.link) { strcpy(a->result, "The card has no link: check the cable or the switch."); }
    else if (kind == 1) {
        net_dhcp_renew();
        /* The lease arrives through net_poll(), which the compositor runs; wait here, with the window on
         * screen, rather than pretending the address is available before the answer is. */
        for (int i = 0; i < 1200 && ni.lease != NET_LEASE_BOUND && ni.lease != NET_LEASE_FAILED; i++) {
            sleep_ms(10);
            net_get_info(&ni);
        }
        if (ni.lease == NET_LEASE_BOUND) {
            char ip[20];
            net_format_ip(ip, sizeof ip, ni.ip);
            strcpy(a->result, "Lease bound: ");
            strcat(a->result, ip);
            a->result_ok = 1;
        } else {
            strcpy(a->result, "No answer to the lease request: see the counters.");
        }
    } else if (kind == 2) {
        if (!ni.gateway) { strcpy(a->result, "No gateway: set an address or take a lease first."); }
        else {
            char ip[20];
            u32 rtt = 0;
            net_format_ip(ip, sizeof ip, ni.gateway);
            if (net_ping(ni.gateway, 1500, &rtt)) {
                a->rtt_ms = rtt;
                strcpy(a->result, "Echo answered by ");
                strcat(a->result, ip);
                strcat(a->result, " in ");
                char ms[16];
                fmt_u32(ms, rtt);
                strcat(a->result, ms);
                strcat(a->result, " ms");
                a->result_ok = 1;
            } else {
                strcpy(a->result, "No echo from ");
                strcat(a->result, ip);
                strcat(a->result, " within 1500 ms");
            }
        }
    } else {
        static char response[4096];
        size_t len = 0;
        char url[64];
        strcpy(url, "http://" NET_FETCH_HOST "/");
        if (net_http_get(url, response, sizeof response, &len) && len) {
            /* The first line of the answer is its status line: quote it, not a summary of it. */
            size_t i = 0;
            while (i < len && response[i] != '\r' && response[i] != '\n' && i < sizeof a->result - 1) i++;
            memcpy(a->result, response, i);
            a->result[i] = 0;
            a->bytes = (u32)len;
            a->result_ok = 1;
        } else {
            strcpy(a->result, "The fetch failed or returned nothing; the counters say how far it got.");
        }
    }
    wm_redraw(w);
}

static void na_mouse(struct window *w, struct mouse_event *e, int x, int y)
{
    struct netapp *a = &na_state;

    a->hover_btn = -1;
    for (int i = 0; i < 3; i++) {
        int bx, by, bw = button_rect(i, w->surf.w, &bx, &by);
        if (x >= bx && x < bx + bw && y >= by && y < by + 26) a->hover_btn = i;
    }
    if (e->buttons & 1) {
        if (a->pressed_btn < 0 && a->hover_btn >= 0) a->pressed_btn = a->hover_btn;
    } else if (a->pressed_btn >= 0) {
        int clicked = a->pressed_btn;
        a->pressed_btn = -1;
        if (clicked == a->hover_btn) na_run(w, clicked + 1);
    }
    wm_redraw(w);
}

static void na_tick(struct window *w)
{
    if ((tick_count % 100) == 0) wm_redraw(w);      /* a second: counters and link state move */
}

struct app app_net = {
    .desktop_label = "Network",
    .id = "net", .title = "Network", .icon = ICON_NET, .single = 1,
    .def_w = 560, .def_h = 360, .min_w = 420, .min_h = 320,
    .paint = na_paint, .key = na_key, .mouse = na_mouse, .tick = na_tick,
};

SCOS_APP(app_net, 013);
