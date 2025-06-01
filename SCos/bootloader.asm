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
    
    ; Reset disk system first with retries
    mov cx, 3           ; Try 3 times
reset_loop:
    mov ah, 0x00
    mov dl, [boot_drive]
    int 0x13
    jnc reset_ok
    loop reset_loop
    jmp disk_error
    
reset_ok:
    ; Load kernel in smaller chunks to be more reliable
    mov bx, KERNEL_OFFSET   ; Load address
    mov cx, 50              ; Total sectors to read
    mov dx, 2               ; Starting sector
    
read_loop:
    push cx                 ; Save sector count
    push dx                 ; Save current sector
    
    ; Read one sector at a time for reliability
    mov ah, 0x02            ; Read sectors function
    mov al, 1               ; Read 1 sector
    mov ch, 0               ; Cylinder 0
    mov cl, dl              ; Current sector
    mov dh, 0               ; Head 0
    mov dl, [boot_drive]    ; Drive
    int 0x13
    
    jc read_error
    
    ; Move to next sector
    pop dx                  ; Restore current sector
    pop cx                  ; Restore sector count
    inc dx                  ; Next sector
    add bx, 512             ; Next memory location
    loop read_loop
    
    ; Verify we actually read something
    mov bx, KERNEL_OFFSET
    cmp word [bx], 0
    je disk_error           ; If first word is 0, kernel probably wasn't loaded
    jmp switch_to_32bit

read_error:
    pop dx                  ; Clean stack
    pop cx
    jmp disk_error

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