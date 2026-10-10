; Real-mode loader. BIOS enters here with DL = boot drive.
bits 16
org 0x7c00

KERNEL_LBA equ 0x1a0
KERNEL_SECTORS equ 0x1a4
ENTRY_POINT equ 0x1a8
DRIVE_BYTE equ 0x1ac

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    mov [0x7c00 + DRIVE_BYTE], dl
    cld

    mov si, dap
    mov word [dap], 0x0010
    mov word [dap + 2], 1
    mov word [dap + 4], 0
    mov dword [dap + 12], 0

    mov eax, [0x7c00 + KERNEL_LBA]
    mov ecx, [0x7c00 + KERNEL_SECTORS]
    mov di, 0x1000
.load:
    test ecx, ecx
    jz .loaded
    mov [dap + 6], di
    mov [dap + 8], eax
    mov dl, [0x7c00 + DRIVE_BYTE]
    mov ah, 0x42
    int 0x13
    jc fail
    add di, 0x20
    inc eax
    dec ecx
    jmp .load

.loaded:
    cli
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:copy_high

bits 32
copy_high:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000
    mov esi, 0x10000
    mov edi, 0x100000
    mov ecx, [0x7c00 + KERNEL_SECTORS]
    shl ecx, 7
    rep movsd
    mov edx, [0x7c00 + ENTRY_POINT]
    mov eax, 0xD15C0001
    xor ebx, ebx
    jmp edx

bits 16
fail:
    mov dx, 0x3f8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 1
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov dx, 0x3f8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x3f8 + 4
    mov al, 0x03
    out dx, al
    mov al, 'D'
    mov dx, 0x3f8
    out dx, al
.hang:
    cli
    hlt
    jmp .hang

align 8
gdt:
    dq 0
    dw 0xffff
    dw 0
    db 0
    db 0x9a
    db 0xcf
    db 0
    dw 0xffff
    dw 0
    db 0
    db 0x92
    db 0xcf
    db 0
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

dap:
    times 16 db 0

times 0x1a0 - ($ - $$) db 0
    dd 64
    dd 1
    dd 0x100020
    db 0x80
times 510 - ($ - $$) db 0
dw 0xaa55
