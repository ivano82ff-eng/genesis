; Flat program. The kernel calls this as void hello(void (*print)(const char *)).
bits 64

push rbx
mov rbx, rdi
lea rdi, [rel msg]
call rbx
pop rbx
ret

msg:
db "hello from program", 0
