bits 64
global isr_irq0
extern timer_tick
extern task_switch

; Save every general register, run the tick, then let the scheduler pick
; which task's saved frame to resume (possibly a different stack).
isr_irq0:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call timer_tick
    mov rdi, rsp
    call task_switch
    mov rsp, rax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; ---- CPU exceptions and the int 0x80 system call ----
; Same register order as isr_irq0, then vector and error code: struct trap_frame.
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
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp
    call fault_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; ---- user stub page: copied to USER_BASE in every user address space ----
; Programs get these as print/sleep/sym; each one is a system call.
section .rodata
align 4096
global user_stub_start, user_stub_end, stub_print, stub_sleep, stub_sym, stub_exit
user_stub_start:
stub_print:              ; void print(const char *text)
    mov eax, 1
    int 0x80
    ret
stub_sleep:              ; void sleep(unsigned ticks)
    mov eax, 2
    int 0x80
    ret
stub_sym:                ; void *sym(const char *lib, const char *name)
    push rbx
    push r12
    push r13
    mov r12, rsi
    mov eax, 3           ; map the library, rax = its user address
    int 0x80
    test rax, rax
    jz .out
    mov rdi, r12
    lea rsi, [rel stub_sym]
    lea rax, [rax + 4]   ; GLIB magic, then jmp lib_lookup
    call rax             ; lookup(sym, import) runs in user mode
.out:
    pop r13
    pop r12
    pop rbx
    ret
stub_exit:               ; the return address of a program's entry
    mov edi, eax
    xor eax, eax
    int 0x80
    jmp stub_exit
user_stub_end:
