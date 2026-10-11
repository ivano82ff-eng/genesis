; Flat program for background tasks (`spawn ticker`). The kernel calls it as
; void ticker(void (*print)(const char *), void (*sleep)(unsigned ticks)).
; Prints "tick" five times, half a second apart, then returns.
bits 64

push rbx
push r12
push r13
mov rbx, rdi
mov r13, rsi
mov r12, 5
.again:
lea rdi, [rel msg]
call rbx
mov edi, 50
call r13
dec r12
jnz .again
pop r13
pop r12
pop rbx
ret

msg:
db "tick", 10, 0
