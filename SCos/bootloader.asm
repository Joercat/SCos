[BITS 16]
[ORG 0x7C00]

; Save boot drive
mov [boot_drive], dl

; Set up stack
mov ax, 0x9000
mov ss, ax
mov sp, 0xFFFF

; Clear screen
mov ax, 0x0003
int 0x10

; Print boot message
mov si, boot_msg
call print_string

; Reset disk system
mov ah, 0x00
mov dl, 0x80
int 0x13
jc disk_error

; Load kernel from disk
mov ah, 0x02     ; Read disk sectors
mov al, 50       ; Number of sectors to read (25KB kernel)
mov ch, 0        ; Cylinder 0
mov cl, 2        ; Start from sector 2 (after bootloader)
mov dh, 0        ; Head 0
mov dl, [boot_drive] ; Use the drive we booted from
mov bx, 0x1000   ; Load kernel at 0x1000
int 0x13         ; Call BIOS disk interrupt
jc disk_error    ; Jump if carry flag set (error)

; Verify kernel loaded by checking first few bytes
mov si, 0x1000
mov al, [si]
cmp al, 0
je disk_error       ; If first byte is 0, kernel didn't load

; Show first few bytes for debugging
mov si, 0x1000
mov al, [si]
mov ah, 0x0E
add al, 0x30        ; Convert to ASCII (assuming small numbers)
int 0x10

mov al, [si+1]
mov ah, 0x0E
add al, 0x30
int 0x10

mov al, [si+2]
mov ah, 0x0E
add al, 0x30
int 0x10

; Print kernel loaded message
mov si, kernel_msg
call print_string

; Switch to protected mode
cli                 ; Disable interrupts
lgdt [gdt_descriptor]

; Enable A20 line
mov ax, 0x2401
int 0x15

; Set PE bit in CR0
mov eax, cr0
or eax, 1
mov cr0, eax

; Jump to protected mode
jmp 0x08:protected_mode

[BITS 32]
protected_mode:
    ; Set up segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up stack
    mov esp, 0x90000

    ; Clear screen in protected mode
    mov edi, 0xB8000
    mov ecx, 80*25
    mov ax, 0x0720  ; Space with white on black
    rep stosw

    ; Print protected mode message
    mov esi, pmode_msg
    mov edi, 0xB8000
    mov ah, 0x0F
    call print_string_32

    ; Small delay to show message
    mov ecx, 5000000
delay_loop:
    dec ecx
    jnz delay_loop

    ; Print jumping message
    mov esi, jump_msg
    mov edi, 0xB8000
    add edi, 160    ; Next line
    mov ah, 0x0F
    call print_string_32

    ; Another small delay
    mov ecx, 2000000
delay_loop2:
    dec ecx
    jnz delay_loop2

    ; Jump to kernel entry point
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

; GDT
gdt_start:
    dd 0x0, 0x0                 ; Null descriptor

gdt_code:
    dw 0xFFFF                   ; Limit 0-15
    dw 0x0000                   ; Base 0-15
    db 0x00                     ; Base 16-23
    db 10011010b                ; Access byte
    db 11001111b                ; Flags + Limit 16-19
    db 0x00                     ; Base 24-31

gdt_data:
    dw 0xFFFF                   ; Limit 0-15
    dw 0x0000                   ; Base 0-15
    db 0x00                     ; Base 16-23
    db 10010010b                ; Access byte
    db 11001111b                ; Flags + Limit 16-19
    db 0x00                     ; Base 24-31

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; Messages
boot_msg db 'SCos Bootloader Starting...', 13, 10, 0
kernel_msg db 'Kernel loaded, switching to protected mode...', 13, 10, 0
pmode_msg db 'Protected mode active, jumping to kernel...', 0
jump_msg db 'Executing kernel jump...', 0
error_msg db 'Disk read error!', 13, 10, 0

boot_drive db 0          ; Variable to store boot drive

; Boot signature
times 510-($-$$) db 0    ; Fill remainder with zeros
db 0x55, 0xAA            ; Boot signature