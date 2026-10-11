; Flat program for background tasks (`spawn pulse`). The kernel calls it as
; void ticker(void (*print)(const char *), void (*sleep)(unsigned ticks)).
; Prints "pulse" every second until `kill`. Used by the desktop core tests.
bits 64

push rbx
push r12
push r13
mov rbx, rdi
mov r13, rsi
mov r12, 0x7fffffff
.again:
lea rdi, [rel msg]
call rbx
mov edi, 100
call r13
dec r12
jnz .again
pop r13
pop r12
pop rbx
ret

msg:
db "pulse", 10, 0
