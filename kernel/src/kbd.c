/* SCos native - PS/2 keyboard driver (scancode set 1) */
#include "scos.h"

volatile u32 input_last_tick;   /* any kbd/mouse event stamps this */
int input_guard_armed;            /* set when WM starts; ignores boot-time BAT/ACK */

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
    if (input_guard_armed) input_last_tick = tick_count;
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

static void kbd_sc(u8 sc)
{
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

/* bounded wait for the controller input buffer to empty */
static int kbd_wait_write(void)
{
    for (int i = 0; i < 200000; i++)
        if ((inb(KBD_STAT) & 2) == 0) return 1;
    return 0;
}
static int kbd_wait_read(void)
{
    for (int i = 0; i < 200000; i++)
        if (inb(KBD_STAT) & 1) return 1;
    return 0;
}
static void kbd_flush(void)
{
    for (int i = 0; i < 64 && (inb(KBD_STAT) & 1); i++) inb(KBD_DATA);
}

/*
 * Real-hardware 8042 init. Firmware commonly hands over with the keyboard
 * port or its IRQ disabled, stale bytes in the output buffer and both
 * devices in an unknown state - v86 forgives all of that, a PC does not.
 */
/* HID boot-protocol usage -> scancode set 1 make code */
static u8 hid_make(u8 u)
{
    if (u >= 0x04 && u <= 0x1D) {
        static const u8 let[26] = {
            0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,
            0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,
            0x15,0x2C };
        return let[u - 0x04];
    }
    if (u >= 0x1E && u <= 0x27) return (u8)(u - 0x1C);
    switch (u) {
    case 0x28: return 0x1C;   /* enter */
    case 0x29: return 0x01;   /* esc */
    case 0x2A: return 0x0E;   /* backspace */
    case 0x2B: return 0x0F;   /* tab */
    case 0x2C: return 0x39;   /* space */
    case 0x2D: return 0x0C;
    case 0x2E: return 0x0D;
    case 0x2F: return 0x1A;
    case 0x30: return 0x1B;
    case 0x31: return 0x2B;
    case 0x33: return 0x27;
    case 0x34: return 0x28;
    case 0x35: return 0x29;
    case 0x36: return 0x33;
    case 0x37: return 0x34;
    case 0x38: return 0x35;
    case 0x39: return 0x3A;   /* caps lock */
    case 0x49: return 0x52;   /* insert */
    case 0x4A: return 0x53;   /* delete */
    case 0x4B: return 0x47;   /* home */
    case 0x4C: return 0x4F;   /* end */
    case 0x4D: return 0x49;   /* pgup */
    case 0x4E: return 0x51;   /* pgdn */
    case 0x4F: return 0x4D;   /* right */
    case 0x50: return 0x4B;   /* left */
    case 0x51: return 0x50;   /* down */
    case 0x52: return 0x48;   /* up */
    default:
        if (u >= 0x3A && u <= 0x45)
            return (u <= 0x44) ? (u8)(u - 0x3A + 0x3B) : 0x58;
        return 0;
    }
}

/* USB HID boot keyboard report -> key events (diffed against previous) */
void kbd_inject_hid(u8 mod, const u8 *keys, u8 *prev_keys, u8 *prev_mod)
{
    static const u8 modmake[8] = { 0x1D,0x2A,0x38,0x5B,0x1D,0x2A,0x38,0x5B };
    for (int b = 0; b < 8; b++) {
        u8 bit = (u8)(1 << b);
        if ((mod & bit) != (*prev_mod & bit))
            kbd_sc((u8)(modmake[b] | ((mod & bit) ? 0 : 0x80)));
    }
    *prev_mod = mod;
    shift_on = (mod & 0x22) ? 1 : 0;
    ctrl_on  = (mod & 0x11) ? 1 : 0;
    alt_on   = (mod & 0x44) ? 1 : 0;
    for (int i = 0; i < 6; i++) {
        u8 u = prev_keys[i];
        if (!u) continue;
        int still = 0;
        for (int j = 0; j < 6; j++) if (keys[j] == u) still = 1;
        if (!still) { u8 mk = hid_make(u); if (mk) kbd_sc((u8)(mk | 0x80)); }
    }
    for (int i = 0; i < 6; i++) {
        u8 u = keys[i];
        if (!u) continue;
        int was = 0;
        for (int j = 0; j < 6; j++) if (prev_keys[j] == u) was = 1;
        if (!was) { u8 mk = hid_make(u); if (mk) kbd_sc(mk); }
    }
    for (int i = 0; i < 6; i++) prev_keys[i] = keys[i];
}

static void kbd_irq(struct regs *r)
{
    (void)r;
    kbd_sc(inb(KBD_DATA));
}

void kbd_init(void)
{
    outb(KBD_STAT, 0xAD);                 /* disable keyboard port */
    outb(KBD_STAT, 0xA7);                 /* disable aux (mouse) port */
    kbd_flush();

    kbd_wait_write();
    outb(KBD_STAT, 0x20);                 /* read controller command byte */
    u8 cmd = 0x00;
    if (kbd_wait_read()) cmd = inb(KBD_DATA);
    cmd |= 0x01;                          /* keyboard IRQ1 enable */
    cmd |= 0x02;                          /* aux IRQ12 enable */
    cmd &= ~0x10;                         /* keyboard clock enabled */
    cmd &= ~0x20;                         /* aux clock enabled */
    /* bit 6 (translation) left exactly as firmware configured it */
    kbd_wait_write();
    outb(KBD_STAT, 0x60);
    kbd_wait_write();
    outb(KBD_DATA, cmd);

    kbd_wait_write();
    outb(KBD_STAT, 0xAE);                 /* enable keyboard port */
    kbd_flush();

    /* keyboard reset + enable scanning; tolerate devices that stay silent */
    kbd_wait_write();
    outb(KBD_DATA, 0xFF);
    if (kbd_wait_read()) {
        inb(KBD_DATA);                    /* ACK / BAT result */
        kbd_flush();
    }
    kbd_wait_write();
    outb(KBD_DATA, 0xF4);                 /* enable scanning */
    if (kbd_wait_read()) inb(KBD_DATA);
    kbd_flush();

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
