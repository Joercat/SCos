/* SCos native - PS/2 mouse driver (with IntelliMouse wheel enable attempt) */
#include "scos.h"

#define MOUSE_DATA 0x60
#define MOUSE_STAT 0x64
#define MOUSE_CMD  0x64

#define QUEUE 64

static struct mouse_event queue[QUEUE];
static int q_head, q_tail;

static u8 packet[4];
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

static void mouse_enqueue(struct mouse_event *e)
{
    int next = (q_head + 1) % QUEUE;
    if (next == q_tail) return;
    queue[q_head] = *e;
    q_head = next;
}

static void handle_packet(void)
{
    if (packet[0] & 0xC0) return;                     /* overflow: drop */
    i32 dx = (i32)(i8)packet[1];
    i32 dy = (i32)(i8)packet[2];
    if (packet[0] & 0x10) dx |= ~0xFF;                /* sign extend safety */
    if (packet[0] & 0x20) dy |= ~0xFF;

    struct mouse_event e;
    e.buttons = packet[0] & 7;
    e.button = 0;
    e.down = 0;

    if (dx || dy) {
        e.type = MEV_MOVE;
        e.dx = (i16)dx; e.dy = (i16)dy; e.wheel = 0;
        mouse_enqueue(&e);
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
    if (packet_len == 4 && packet[3]) {
        e.type = MEV_WHEEL;
        e.wheel = (i8)packet[3];
        e.dx = 0; e.dy = 0;
        mouse_enqueue(&e);
    }
    button_state = e.buttons;
}

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

void mouse_init(void)
{
    /* enable aux port on the 8042 */
    mouse_wait_write();
    outb(MOUSE_CMD, 0xA8);

    /* enable IRQ12, translation */
    mouse_wait_write();
    outb(MOUSE_CMD, 0x20);
    mouse_wait_read();
    u8 status = inb(MOUSE_DATA);
    status = (status | 0x02) & ~0x20;   /* IRQ12 on, mouse not inhibited */
    mouse_wait_write();
    outb(MOUSE_CMD, 0x60);
    mouse_wait_write();
    outb(MOUSE_DATA, status);

    /* reset + enable data reporting */
    mouse_write(0xFF);
    mouse_read(); mouse_read(); mouse_read();
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

    mouse_write(0xF4);                     /* enable streaming */
    mouse_read();

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
