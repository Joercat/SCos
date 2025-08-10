global idt_load
global keyboard_interrupt_wrapper

extern keyboard_handler

idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret


keyboard_interrupt_wrapper:
    pusha                    
    call keyboard_handler   
    
    mov al, 0x20
    out 0x20, al
    popa                     
    iret                     
