/* SCos native - PS/2 mouse driver (with IntelliMouse wheel enable attempt) */
#include "scos.h"

#define MOUSE_DATA 0x60
#define MOUSE_STAT 0x64
#define MOUSE_CMD  0x64

#define QUEUE 128

static struct mouse_event queue[QUEUE];
static int q_head, q_tail;

static u8 packet[4];
static int mouse_ok;
static int packet_idx;
static int packet_len = 3;
static u8 button_state;

static void mouse_wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if ((inb(MOUSE_STAT) & 2) == 0) return;
}
static void mouse_wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(MOUSE_STAT) & 1) return;
}
static void mouse_write(u8 b)
{
    mouse_wait_write();
    outb(MOUSE_CMD, 0xD4);
    mouse_wait_write();
    outb(MOUSE_DATA, b);
}
static u8 mouse_read(void)
{
    mouse_wait_read();
    return inb(MOUSE_DATA);
}

u32 input_mouse_enqueued, input_mouse_dropped;

static void mouse_enqueue(struct mouse_event *e)
{
    int next = (q_head + 1) % QUEUE;
    if (next == q_tail) { input_mouse_dropped++; return; }
    input_mouse_enqueued++;
    queue[q_head] = *e;
    q_head = next;
}

static void mouse_apply(u8 btns, i32 dx, i32 dy, i32 wheel)
{
    if (input_guard_armed && (btns || dx || dy || wheel))
        input_last_tick = tick_count;
    i32 unused = 0;
    (void)unused;
    u8 packet0_btns = btns;
    i32 pdx = dx, pdy = dy;
    struct mouse_event e;
    e.buttons = packet0_btns & 7;
    e.button = 0;
    e.down = 0;

    if (pdx || pdy) {
        e.type = MEV_MOVE;
        /* r38: coalesce into the last QUEUED move when one is pending.
         * A 1000 Hz mouse produces up to 10 moves per WM tick while the
         * queue holds 128 events and mouse_enqueue DROPS whatever
         * arrives at a full queue - move floods were evicting button
         * transitions (the r37 field cluster: clicks needing spam,
         * double-clicks missed, stuck highlights from lost releases).
         * Summing pending deltas keeps the trajectory pixel-exact (the
         * WM applies moves additively) and guarantees a move flood can
         * never displace a button or wheel event. */
        int merged = 0;
        if (q_head != q_tail) {
            struct mouse_event *L = &queue[(q_head - 1 + QUEUE) % QUEUE];
            if (L->type == MEV_MOVE) {
                i32 sx = (i32)L->dx + dx, sy = (i32)L->dy + dy;
                if (sx > 32767) sx = 32767;
                if (sx < -32768) sx = -32768;
                if (sy > 32767) sy = 32767;
                if (sy < -32768) sy = -32768;
                L->dx = (i16)sx;
                L->dy = (i16)sy;
                merged = 1;
            }
        }
        if (!merged) {
            e.dx = (i16)pdx; e.dy = (i16)pdy; e.wheel = 0;
            mouse_enqueue(&e);
        }
    }
    for (int b = 0; b < 3; b++) {
        u8 bit = 1 << b;
        if ((button_state & bit) != (e.buttons & bit)) {
            e.type = MEV_BUTTON;
            e.button = bit;
            e.down = (e.buttons & bit) ? 1 : 0;
            e.dx = 0; e.dy = 0; e.wheel = 0;
            mouse_enqueue(&e);
        }
    }
    if (wheel) {
        e.type = MEV_WHEEL;
        e.wheel = (i8)wheel;
        e.dx = 0; e.dy = 0;
        mouse_enqueue(&e);
    }
    button_state = e.buttons;
}

static void handle_packet(void)
{
    if (packet[0] & 0xC0) return;                     /* overflow: drop */
    i32 dx = (i32)(i8)packet[1];
    i32 dy = (i32)(i8)packet[2];
    if (packet[0] & 0x10) dx |= ~0xFF;                /* sign extend safety */
    if (packet[0] & 0x20) dy |= ~0xFF;
    mouse_apply(packet[0] & 7, dx, dy,
                packet_len == 4 ? (i32)(i8)packet[3] : 0);
}

/* USB HID boot mouse reports arrive here */
void mouse_inject(u8 buttons, i32 dx, i32 dy, i32 wheel)
{
    /* r35 ROOT FIX - inverted vertical axis.  HID reports dy POSITIVE =
     * physical DOWN, but the rest of SCos (the mouse_apply queue and the
     * WM's `my -= e->dy`) runs on the PS/2 convention where positive dy
     * = UP.  Passing HID dy through unflipped inverts the whole vertical
     * axis on screen - the r34 field symptom: "moving left went
     * diagonally DOWN-left", "no way to move the mouse up", "stuck under
     * the screen" (physical up drove the cursor down into the bottom
     * edge; the one time it moved "up" was a physical DOWN move).  The
     * wheel sign already matches between HID and PS/2 (Linux maps both
     * to REL_WHEEL unflipped), so only dy flips. */
    mouse_apply(buttons, dx, -dy, wheel);
}

int mouse_present(void) { return mouse_ok; }

static void mouse_irq(struct regs *r)
{
    (void)r;
    u8 status = inb(MOUSE_STAT);
    if (!(status & 1)) return;
    u8 data = inb(MOUSE_DATA);
    if (!(status & 0x20)) return;                     /* not aux data */

    if (packet_idx == 0 && (data & 0x08) == 0) return;  /* resync */
    packet[packet_idx++] = data;
    if (packet_idx == packet_len) {
        packet_idx = 0;
        handle_packet();
    }
}

static int mouse_cmd_ack(u8 cmd)
{
    mouse_write(cmd);
    for (int i = 0; i < 4; i++) {
        mouse_wait_read();
        u8 r = inb(MOUSE_DATA);
        if (r == 0xFA) return 1;          /* ACK */
        if (r == 0xFE) return 0;          /* NAK */
    }
    return 0;
}

void mouse_init(void)
{
    /* enable aux port on the 8042 */
    mouse_wait_write();
    outb(MOUSE_CMD, 0xA8);

    /* enable IRQ12, keep firmware translation setting, unlock aux clock */
    mouse_wait_write();
    outb(MOUSE_CMD, 0x20);
    mouse_wait_read();
    u8 status = inb(MOUSE_DATA);
    status = (status | 0x02) & ~0x20;   /* IRQ12 on, mouse not inhibited */
    mouse_wait_write();
    outb(MOUSE_CMD, 0x60);
    mouse_wait_write();
    outb(MOUSE_DATA, status);

    /* reset; if nothing ACKs there is no mouse - leave the port quiet */
    if (!mouse_cmd_ack(0xFF)) {
        klog("mouse: no ACK on reset, aux port left idle");
        return;
    }
    while (inb(MOUSE_STAT) & 1) inb(MOUSE_DATA);   /* BAT + id bytes */
    mouse_write(0xF6);
    mouse_read();

    /* try to enable wheel (IntelliMouse) */
    mouse_write(0xF3); mouse_read();
    mouse_write(200);  mouse_read();
    mouse_write(0xF3); mouse_read();
    mouse_write(100);  mouse_read();
    mouse_write(0xF3); mouse_read();
    mouse_write(80);   mouse_read();
    mouse_write(0xF2); mouse_read();
    u8 id = mouse_read();
    if (id == 3) {
        /* try the 5-button (IntelliMouse Explorer) sequence so that both
         * v86 and spec-compliant real mice end up with 4-byte packets */
        mouse_write(0xF3); mouse_read();
        mouse_write(200);  mouse_read();
        mouse_write(0xF3); mouse_read();
        mouse_write(200);  mouse_read();
        mouse_write(0xF3); mouse_read();
        mouse_write(80);   mouse_read();
        mouse_write(0xF2); mouse_read();
        id = mouse_read();
    }
    if (id >= 3) packet_len = 4;
    klog("mouse: device id %d, packet len %d", id, packet_len);

    while (inb(MOUSE_STAT) & 1) inb(MOUSE_DATA);   /* flush probe leftovers */
    if (!mouse_cmd_ack(0xF4)) {            /* enable streaming */
        klog("mouse: streaming enable refused");
        return;
    }
    mouse_ok = 1;

    irq_install(12, mouse_irq);
    pic_clear_mask(12);
    pic_clear_mask(2);                     /* cascade */
}

int mouse_poll(struct mouse_event *out)
{
    if (q_tail == q_head) return 0;
    *out = queue[q_tail];
    q_tail = (q_tail + 1) % QUEUE;
    return 1;
}
