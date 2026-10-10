; Multiboot2 entry in protected mode, then long mode.
; QEMU -kernel rejects ELF64, so GRUB loads this image.

MULTIBOOT2_MAGIC equ 0xe85250d6
MULTIBOOT2_ARCH  equ 0

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
    ; EBX is the Multiboot2 info pointer. CPUID later overwrites EBX.
    mov dword [mb_info_ptr], ebx
    cli
    cld
    mov esp, stack_top
    ; EAX is the Multiboot2 magic. Do not touch it before the check.
    call check_multiboot
    call check_cpuid
    call check_long_mode
    call set_up_page_tables
    call enable_long_mode
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

check_multiboot:
    cmp eax, 0x36d76289
    je .ok
    cmp eax, 0xD15C0001
    jne .bad
    mov dword [mb_info_ptr], 0
.ok:
    ret
.bad:
    mov al, "M"
    jmp boot_error

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .bad
    ret
.bad:
    mov al, "C"
    jmp boot_error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .bad
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .bad
    ret
.bad:
    mov al, "L"
    jmp boot_error

set_up_page_tables:
    mov edi, p4_table
    xor eax, eax
    mov ecx, 4096 * 6 / 4
    rep stosd

    mov eax, p3_table
    or eax, 0b11
    mov dword [p4_table], eax
    mov dword [p4_table + 4], 0

    xor ebx, ebx
.next_pd:
    mov eax, p2_tables
    mov edx, ebx
    shl edx, 12
    add eax, edx
    or eax, 0b11
    mov dword [p3_table + ebx * 8], eax
    mov dword [p3_table + ebx * 8 + 4], 0

    xor ecx, ecx
.next_pde:
    mov eax, ebx
    shl eax, 9
    add eax, ecx
    mov edx, 0x200000
    mul edx
    or eax, 0b10000011
    mov edi, p2_tables
    mov esi, ebx
    shl esi, 12
    add edi, esi
    mov dword [edi + ecx * 8], eax
    mov dword [edi + ecx * 8 + 4], edx
    inc ecx
    cmp ecx, 512
    jne .next_pde

    inc ebx
    cmp ebx, 4
    jne .next_pd
    ret

enable_long_mode:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

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

bits 64
long_mode_start:
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Temporary stack below 1MiB while BSS (including stack) is cleared.
    mov rsp, 0x90000
    extern bss_start
    extern bss_end
    mov rdi, bss_start
    mov rcx, bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb
    mov rsp, stack_top
    mov edi, [mb_info_ptr]
    call kmain
    cli
.hang:
    hlt
    jmp .hang

section .data
align 8
global mb_info_ptr
mb_info_ptr:
    dq 0

section .rodata
align 8
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.data: equ $ - gdt64
    dq (1 << 41) | (1 << 44) | (1 << 47)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; Page tables live in .data so clearing BSS after long mode stays safe.
section .data
align 4096
p4_table:
    times 4096 db 0
p3_table:
    times 4096 db 0
p2_tables:
    times 4096 * 4 db 0

section .bss
align 4096
stack_bottom:
    resb 4096 * 8
stack_top:
