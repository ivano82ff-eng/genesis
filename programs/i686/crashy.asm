; i686 twin of programs/crashy.asm (see there): crashy(print, sleep, sym, args).
bits 32

push ebx
push esi
push edi
mov ebx, [esp + 16]     ; print
mov esi, [esp + 28]     ; args
call base
base:
pop edi                 ; edi = runtime address of base
test esi, esi
jz usage
mov eax, [esi]
cmp eax, 'null'
je do_null
cmp eax, 'kern'
je do_kernel
cmp eax, 'div'
je do_div
cmp eax, 'op'
je do_op
cmp eax, 'sys'
je do_sys
cmp eax, 'loop'
je do_loop
usage:
lea eax, [edi + m_usage - base]
call say
pop edi
pop esi
pop ebx
ret

say:                    ; print(eax)
push eax
call ebx
add esp, 4
ret

do_null:
lea eax, [edi + m_null - base]
call say
xor eax, eax
mov dword [eax], 1
jmp usage

do_kernel:
lea eax, [edi + m_kernel - base]
call say
mov eax, 0x100000
mov dword [eax], 0xdead
jmp usage

do_div:
lea eax, [edi + m_div - base]
call say
xor edx, edx
mov eax, 1
xor ecx, ecx
div ecx
jmp usage

do_op:
lea eax, [edi + m_op - base]
call say
ud2

do_sys:
lea eax, [edi + m_sys - base]
call say
mov eax, 0x100000
call say
jmp usage

do_loop:
lea eax, [edi + m_loop - base]
call say
.spin:
jmp .spin

m_usage: db "crashy: null | kernel | div | op | sys | loop", 10, 0
m_null:   db "crashy: writing to address 0", 10, 0
m_kernel: db "crashy: writing into kernel memory", 10, 0
m_div:    db "crashy: dividing by zero", 10, 0
m_op:     db "crashy: running an invalid instruction", 10, 0
m_sys:    db "crashy: asking the kernel to print kernel memory", 10, 0
m_loop:   db "crashy: spinning forever (kill me)", 10, 0
