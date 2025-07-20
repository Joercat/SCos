global idt_load
global keyboard_interrupt_wrapper

extern keyboard_handler

idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret

; Keyboard interrupt wrapper (IRQ1)
keyboard_interrupt_wrapper:
    pusha                    ; Save all registers
    call keyboard_handler    ; Call C++ keyboard handler
    ; Send EOI to PIC (End of Interrupt)
    mov al, 0x20
    out 0x20, al
    popa                     ; Restore all registers
    iret                     ; Return from interrupt