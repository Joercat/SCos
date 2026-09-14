/* SCos native - PS/2 keyboard driver (scancode set 1) */
#include "scos.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64
#define QUEUE 256

static struct key_event queue[QUEUE];
static volatile int q_head, q_tail;

static u8 shift_on, ctrl_on, alt_on, caps_on;

static const char sc_ascii[] = {
    /* 0x00 */ 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    /* 0x0f */ '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    /* 0x1d */ 0 /*ctrl*/, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    /* 0x2a */ 0 /*shift*/, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    /* 0x36 */ 0 /*shift*/, '*', 0 /*alt*/, ' ', 0 /*caps*/,
};
static const char sc_shift[] = {
    /* 0x00 */ 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    /* 0x0f */ '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    /* 0x1d */ 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    /* 0x2a */ 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    /* 0x36 */ 0, '*', 0, ' ', 0,
};

static void kbd_enqueue(struct key_event *e)
{
    int next = (q_head + 1) % QUEUE;
    if (next == q_tail) return;      /* full: drop */
    queue[q_head] = *e;
    q_head = next;
}

static u16 special_key(u8 sc)
{
    switch (sc) {
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4b: return KEY_LEFT;
    case 0x4d: return KEY_RIGHT;
    case 0x4f: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x3b: return KEY_F1;
    case 0x3c: return KEY_F2;
    case 0x3d: return KEY_F3;
    case 0x3e: return KEY_F4;
    case 0x3f: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    default:   return KEY_NONE;
    }
}

static void kbd_irq(struct regs *r)
{
    (void)r;
    u8 sc = inb(KBD_DATA);
    struct key_event e;
    e.scancode = sc;
    e.ctrl = ctrl_on; e.alt = alt_on; e.shift = shift_on;
    e.pressed = (sc & 0x80) ? 0 : 1;
    u8 make = sc & 0x7f;

    switch (make) {
    case 0x2a: case 0x36: shift_on = e.pressed; e.keycode = KEY_LSHIFT; kbd_enqueue(&e); return;
    case 0x1d: ctrl_on = e.pressed; e.keycode = KEY_LCTRL; kbd_enqueue(&e); return;
    case 0x38: alt_on = e.pressed; e.keycode = KEY_LALT; kbd_enqueue(&e); return;
    case 0x3a:
        if (e.pressed) caps_on = !caps_on;
        e.keycode = KEY_CAPS; kbd_enqueue(&e); return;
    default: break;
    }
    if (!e.pressed) {
        u16 sp = special_key(make);
        if (sp) { e.keycode = sp; kbd_enqueue(&e); }
        return;
    }

    u16 sp = special_key(make);
    if (sp) { e.keycode = sp; kbd_enqueue(&e); return; }

    if (make < sizeof(sc_ascii)) {
        char c = shift_on ? sc_shift[make] : sc_ascii[make];
        if (c >= 'a' && c <= 'z' && caps_on) c -= 32;
        if (c >= 'A' && c <= 'Z' && caps_on && shift_on) c += 32;
        if (ctrl_on && c >= 'a' && c <= 'z') c -= 96;      /* ctrl+letter */
        e.keycode = (u8)c;
        kbd_enqueue(&e);
    }
}

void kbd_init(void)
{
    while (inb(KBD_STAT) & 1) inb(KBD_DATA);    /* drain */
    irq_install(1, kbd_irq);
    pic_clear_mask(1);
}

int kbd_poll(struct key_event *out)
{
    if (q_tail == q_head) return 0;
    *out = queue[q_tail];
    q_tail = (q_tail + 1) % QUEUE;
    return 1;
}
