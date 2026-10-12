; i686 twin of programs/hello.asm: hello(print) with cdecl arguments.
bits 32

call .here
.here:
pop ecx
lea eax, [ecx + msg - .here]
push eax
call [esp + 8]
add esp, 4
ret

msg:
db "hello from program", 0
