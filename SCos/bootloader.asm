[ORG 0x7C00]
[BITS 16]

KERNEL_OFFSET equ 0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

load_kernel:
    ; Save boot drive number that BIOS gave us
    mov [boot_drive], dl
    
    ; Reset disk system first
    mov ah, 0x00
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    
    ; Try to get drive parameters to ensure drive is accessible
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    
    ; Load kernel - read more sectors to ensure we get the full kernel
    mov bx, KERNEL_OFFSET
    mov ah, 0x02        ; Read sectors function
    mov al, 50          ; Read 50 sectors (25KB) 
    mov ch, 0           ; Cylinder 0
    mov cl, 2           ; Start from sector 2 (after bootloader)
    mov dh, 0           ; Head 0
    mov dl, [boot_drive] ; Use the boot drive BIOS gave us
    int 0x13
    
    jc disk_error
    
    ; Verify we actually read something
    mov ax, KERNEL_OFFSET
    mov es, ax
    mov bx, 0
    mov ax, [es:bx]     ; Read first word instead of byte
    cmp ax, 0
    je disk_error       ; If first word is 0, kernel probably wasn't loaded

switch_to_32bit:
    cli
    lgdt [gdt_descriptor]
    
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    
    jmp CODE_SEG:init_32bit

disk_error:
    mov si, disk_error_msg
    call print_string
    
    ; Wait for keypress
    mov ah, 0x00
    int 0x16
    
    ; Retry loading
    jmp load_kernel

print_string:
    lodsb
    or al, al
    jz done
    mov ah, 0x0E
    int 0x10
    jmp print_string
done:
    ret

[BITS 32]
init_32bit:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    mov ebp, 0x90000
    mov esp, ebp
    
    jmp KERNEL_OFFSET

gdt_start:
    dd 0x0
    dd 0x0

gdt_code:
    dw 0xFFFF
    dw 0x0
    db 0x0
    db 10011010b
    db 11001111b
    db 0x0

gdt_data:
    dw 0xFFFF
    dw 0x0
    db 0x0
    db 10010010b
    db 11001111b
    db 0x0

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

boot_drive db 0
disk_error_msg db 'Disk read error! Press any key to retry...', 0

times 510 - ($ - $$) db 0
dw 0xAA55