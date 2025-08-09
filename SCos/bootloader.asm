[BITS 16]
[ORG 0x7C00]

mov [boot_drive], dl

mov ax, 0x9000
mov ss, ax
mov sp, 0xFFFF

mov ax, 0x0003
int 0x10

mov si, boot_msg
call print_string

mov ah, 0x00
mov dl, 0x80
int 0x13
jc disk_error

mov ah, 0x02
mov al, 50
mov ch, 0
mov cl, 2
mov dh, 0
mov dl, [boot_drive]
mov bx, 0x1000
int 0x13
jc disk_error

mov si, 0x1000
mov al, [si]
cmp al, 0
je disk_error

mov si, 0x1000
mov al, [si]
mov ah, 0x0E
add al, 0x30
int 0x10

mov al, [si+1]
mov ah, 0x0E
add al, 0x30
int 0x10

mov al, [si+2]
mov ah, 0x0E
add al, 0x30
int 0x10

mov si, kernel_msg
call print_string

cli
lgdt [gdt_descriptor]

mov ax, 0x2401
int 0x15

mov eax, cr0
or eax, 1
mov cr0, eax

jmp 0x08:protected_mode

[BITS 32]
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, 0x90000

    mov edi, 0xB8000
    mov ecx, 80*25
    mov ax, 0x0720
    rep stosw

    mov esi, pmode_msg
    mov edi, 0xB8000
    mov ah, 0x0F
    call print_string_32

    mov ecx, 5000000
delay_loop:
    dec ecx
    jnz delay_loop

    mov esi, jump_msg
    mov edi, 0xB8000
    add edi, 160
    mov ah, 0x0F
    call print_string_32

    mov ecx, 2000000
delay_loop2:
    dec ecx
    jnz delay_loop2

    jmp 0x08:0x1000

[BITS 16]
print_string:
    mov ah, 0x0E
.loop:
    lodsb
    cmp al, 0
    je .done
    int 0x10
    jmp .loop
.done:
    ret

[BITS 32]
print_string_32:
    push eax
    push edi
.loop:
    lodsb
    cmp al, 0
    je .done
    mov [edi], al
    mov [edi+1], ah
    add edi, 2
    jmp .loop
.done:
    pop edi
    pop eax
    ret

disk_error:
    mov si, error_msg
    call print_string
    hlt

gdt_start:
    dd 0x0, 0x0

gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

boot_msg db 'SCos Bootloader Starting...', 13, 10, 0
kernel_msg db 'Kernel loaded, switching to protected mode...', 13, 10, 0
pmode_msg db 'Protected mode active, jumping to kernel...', 0
jump_msg db 'Executing kernel jump...', 0
error_msg db 'Disk read error!', 13, 10, 0

boot_drive db 0

times 510-($-$$) db 0
db 0x55, 0xAA
