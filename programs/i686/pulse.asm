; i686 twin of programs/pulse.asm: pulse(print, sleep) with cdecl arguments.
bits 32

push ebx
push esi
push edi
mov ebx, [esp + 16]     ; print
mov esi, [esp + 20]     ; sleep
mov edi, 0x7fffffff
.again:
call .here
.here:
pop eax
add eax, msg - .here
push eax
call ebx
add esp, 4
push 100
call esi
add esp, 4
dec edi
jnz .again
pop edi
pop esi
pop ebx
ret

msg:
db "pulse", 10, 0
