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
