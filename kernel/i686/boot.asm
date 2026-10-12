; i686 entry for the web build (v86 has no long mode): Multiboot2 in
; protected mode, 32-bit paging with 4 MiB pages, identity map of 4 GiB
; except the user window at 1 GiB (see kernel/user.h).

MULTIBOOT2_MAGIC equ 0xe85250d6
MULTIBOOT2_ARCH  equ 0
USER_PDE_FIRST   equ 256
USER_PDE_LAST    equ 257

section .text
align 8
header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd header_end - header_start
    dd -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + (header_end - header_start))
    align 8
    dw 0
    dw 0
    dd 8
header_end:

align 16
bits 32
global _start
extern kmain
_start:
    mov dword [mb_info_ptr], ebx
    cli
    cld
    mov esp, stack_top
    cmp eax, 0x36d76289
    je .mb_ok
    cmp eax, 0xD15C0001
    jne .bad_mb
    mov dword [mb_info_ptr], 0
.mb_ok:
    ; page directory: PDE i -> i * 4 MiB, present + rw + 4 MiB page
    mov edi, page_dir
    xor ecx, ecx
.pde:
    mov eax, ecx
    shl eax, 22
    or eax, 0x83
    cmp ecx, USER_PDE_FIRST
    jb .store
    cmp ecx, USER_PDE_LAST
    ja .store
    xor eax, eax
.store:
    mov [edi + ecx * 4], eax
    inc ecx
    cmp ecx, 1024
    jne .pde

    mov eax, cr4
    or eax, 1 << 4          ; PSE
    mov cr4, eax
    mov eax, page_dir
    mov cr3, eax
    mov eax, cr0
    or eax, 1 << 31         ; PG
    mov cr0, eax

    lgdt [gdt32.pointer]
    jmp 0x08:.flat
.flat:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Temporary stack below 1MiB while BSS (including stack) is cleared.
    mov esp, 0x90000
    extern bss_start
    extern bss_end
    mov edi, bss_start
    mov ecx, bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    mov esp, stack_top
    push dword [mb_info_ptr]
    call kmain
    cli
.hang:
    hlt
    jmp .hang

.bad_mb:
    mov al, "M"
    jmp boot_error

; al = one-character reason. Prints "ERR X" on COM1 and stops.
boot_error:
    mov bl, al
    call serial_init
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f204f52
    mov al, "E"
    call serial_putc
    mov al, "R"
    call serial_putc
    mov al, "R"
    call serial_putc
    mov al, " "
    call serial_putc
    mov al, bl
    call serial_putc
    mov al, 10
    call serial_putc
    cli
.hang:
    hlt
    jmp .hang

serial_init:
    mov dx, 0x3f8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3f8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 0x03
    out dx, al
    mov dx, 0x3f8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3f8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x3f8 + 2
    mov al, 0xc7
    out dx, al
    mov dx, 0x3f8 + 4
    mov al, 0x0b
    out dx, al
    ret

; al = byte
serial_putc:
    push edx
    push eax
    mov dx, 0x3f8 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop eax
    mov dx, 0x3f8
    out dx, al
    pop edx
    ret


section .data
align 8
global mb_info_ptr
mb_info_ptr:
    dd 0

align 8
gdt32:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
.pointer:
    dw $ - gdt32 - 1
    dd gdt32

; In .data so clearing BSS keeps it.
align 4096
global page_dir
page_dir:
    times 4096 db 0

section .bss
align 4096
stack_bottom:
    resb 4096 * 8
stack_top:
