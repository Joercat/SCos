/*
 * SCos native - kernel panic / fatal error screen.
 *
 * A real halt screen: ASCII art, the panic reason, the faulting
 * exception, a full register dump (including EIP/EFLAGS recovered from
 * the interrupt stack frame) and a raw stack dump, then the machine is
 * halted. Reached by hardware exceptions or the terminal 'panic'
 * command (which exists so the screen can be inspected on demand).
 */
#include "scos.h"

static void hx(char *out, u32 v)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out[i] = d[(v >> (28 - i * 4)) & 0xF];
    out[8] = 0;
}

void kernel_panic_info(const char *reason, const char *detail, struct regs *r)
{
    irq_disable();
    if (!screen.px) { for (;;) cpu_hlt(); }

    fb_clear(0x100000);
    /* the user's neofetch logo (art.txt), rendered in panic red */
    int y = 40;
    for (int i = 0; i < neofetch_art_lines; i++) {
        s_text(&screen, 60, y, neofetch_art[i], 0xFF4444);
        y += 18;
    }
    int tx = 60 + (neofetch_art_width + 3) * 8;
    s_text_scaled(&screen, tx, 60, "KERNEL", 0xFF4444, 3);
    s_text_scaled(&screen, tx, 110, "PANIC", 0xFF4444, 3);

    y = 210;
    s_text(&screen, tx, y, reason ? reason : "fatal error", 0xFFFFFF); y += 24;
    if (detail && detail[0]) { s_text(&screen, tx, y, detail, 0xCCCCCC); y += 24; }

    if (r) {
        const u32 *fr = (const u32 *)r;
        u32 eip = fr[10], eflags = fr[12];
        char line[96], a[9], b[9];
        y += 8;
        s_text(&screen, 60, y, "Register dump:", 0xFF9999); y += 20;
        hx(a, r->eax); hx(b, r->ebx);
        strcpy(line, "eax 0x"); strcat(line, a); strcat(line, "   ebx 0x"); strcat(line, b);
        hx(a, r->ecx); hx(b, r->edx);
        strcat(line, "   ecx 0x"); strcat(line, a); strcat(line, "   edx 0x"); strcat(line, b);
        s_text(&screen, 60, y, line, 0xCCCCCC); y += 18;
        hx(a, r->esi); hx(b, r->edi);
        strcpy(line, "esi 0x"); strcat(line, a); strcat(line, "   edi 0x"); strcat(line, b);
        hx(a, r->ebp); hx(b, r->esp);
        strcat(line, "   ebp 0x"); strcat(line, a); strcat(line, "   esp 0x"); strcat(line, b);
        s_text(&screen, 60, y, line, 0xCCCCCC); y += 18;
        hx(a, eip); hx(b, eflags);
        strcpy(line, "eip 0x"); strcat(line, a); strcat(line, "   eflags 0x"); strcat(line, b);
        strcat(line, "   int ");
        char n[8]; fmt_u32(n, r->int_no); strcat(line, n);
        strcat(line, "   err 0x"); hx(a, r->err_code); strcat(line, a);
        s_text(&screen, 60, y, line, 0xCCCCCC); y += 26;

        s_text(&screen, 60, y, "Stack at ESP:", 0xFF9999); y += 20;
        const u32 *sp = (const u32 *)(u32)r->esp;
        for (int row = 0; row < 2; row++) {
            line[0] = 0;
            for (int k = 0; k < 8; k++) {
                hx(a, sp[row * 8 + k]);
                strcat(line, "0x"); strcat(line, a); strcat(line, " ");
            }
            s_text(&screen, 60, y, line, 0xCCCCCC); y += 18;
        }
    }
    y += 16;
    s_text(&screen, 60, y, "The system has been halted. Power off or reset the machine.", 0xFFFFFF);
    y += 20;
    s_text(&screen, 60, y, "If this happened while booting a real device, report the register dump above.", 0x888888);
    fb_flip();
    for (;;) cpu_hlt();
}

void kernel_panic(const char *reason)
{
    kernel_panic_info(reason, NULL, NULL);
}

void kernel_panic_regs(const char *name, struct regs *r)
{
    char detail[96];
    strcpy(detail, "exception ");
    char n[8]; fmt_u32(n, r->int_no); strcat(detail, n);
    strcat(detail, ": "); strncat(detail, name, 40);
    kernel_panic_info("Unhandled processor exception", detail, r);
}
