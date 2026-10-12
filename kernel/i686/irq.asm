; i686 twin of kernel/irq.asm: timer IRQ with task switch, CPU exceptions,
; int 0x80 and the user stub page. Frames: pushad, (vector, error code), iret.
bits 32
global isr_irq0
extern timer_tick
extern task_switch

isr_irq0:
    pushad
    cld
    call timer_tick
    push esp
    call task_switch
    mov esp, eax
    popad
    iretd

extern fault_handler
global isr_syscall

%macro EXC 2
global isr_exc%1
isr_exc%1:
%if %2 == 0
    push 0
%endif
    push %1
    jmp trap_common
%endmacro

EXC 0, 0
EXC 1, 0
EXC 3, 0
EXC 4, 0
EXC 5, 0
EXC 6, 0
EXC 7, 0
EXC 8, 1
EXC 10, 1
EXC 11, 1
EXC 12, 1
EXC 13, 1
EXC 14, 1
EXC 16, 0
EXC 17, 1
EXC 19, 0

isr_syscall:
    push 0
    push 0x80

trap_common:
    pushad
    cld
    push esp
    call fault_handler
    add esp, 4
    popad
    add esp, 8
    iretd

; ---- user stub page: copied to USER_BASE in every user address space ----
; cdecl; the system call takes its number in eax and its argument in edi.
section .rodata
align 4096
global user_stub_start, user_stub_end, stub_print, stub_sleep, stub_sym, stub_exit
user_stub_start:
stub_print:              ; void print(const char *text)
    push edi
    mov edi, [esp + 8]
    mov eax, 1
    int 0x80
    pop edi
    ret
stub_sleep:              ; void sleep(unsigned ticks)
    push edi
    mov edi, [esp + 8]
    mov eax, 2
    int 0x80
    pop edi
    ret
stub_sym:                ; void *sym(const char *lib, const char *name)
    push edi
    mov edi, [esp + 8]
    mov eax, 3           ; map the library, eax = its user address
    int 0x80
    test eax, eax
    jz .out
    call .here
.here:
    pop ecx
    sub ecx, .here - stub_sym
    push ecx             ; import = this stub (user address)
    push dword [esp + 16] ; name
    add eax, 4           ; GLIB magic, then jmp lib_lookup
    call eax             ; lookup(sym, import) runs in user mode
    add esp, 8
.out:
    pop edi
    ret
stub_exit:               ; the return address of a program's entry
    mov edi, eax
    xor eax, eax
    int 0x80
    jmp stub_exit
user_stub_end:
