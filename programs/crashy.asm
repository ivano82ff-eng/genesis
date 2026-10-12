; Flat program that misbehaves on purpose, to show memory protection:
;   run crashy null    write to address 0
;   run crashy kernel  write into kernel memory (1 MiB)
;   run crashy div     divide by zero
;   run crashy op      invalid instruction
;   run crashy sys     hand the kernel a kernel pointer to print
;   spawn crashy loop  spin forever (stop it with kill)
; Called as crashy(print, sleep, sym, args); args is a string or 0.
bits 64

push rbx
push r12
push r13
mov rbx, rdi
mov r12, rcx
test r12, r12
jz usage
mov eax, [r12]
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
; 'div', 'op' and 'sys' are shorter than 4 bytes: compare with their NUL
usage:
lea rdi, [rel m_usage]
call rbx
pop r13
pop r12
pop rbx
ret

do_null:
lea rdi, [rel m_null]
call rbx
xor eax, eax
mov dword [rax], 1
jmp usage

do_kernel:
lea rdi, [rel m_kernel]
call rbx
mov eax, 0x100000
mov dword [rax], 0xdead
jmp usage

do_div:
lea rdi, [rel m_div]
call rbx
xor edx, edx
mov eax, 1
xor ecx, ecx
div ecx
jmp usage

do_op:
lea rdi, [rel m_op]
call rbx
ud2

do_sys:
lea rdi, [rel m_sys]
call rbx
mov edi, 0x100000
call rbx
jmp usage

do_loop:
lea rdi, [rel m_loop]
call rbx
.spin:
jmp .spin

m_usage: db "crashy: null | kernel | div | op | sys | loop", 10, 0
m_null:   db "crashy: writing to address 0", 10, 0
m_kernel: db "crashy: writing into kernel memory", 10, 0
m_div:    db "crashy: dividing by zero", 10, 0
m_op:     db "crashy: running an invalid instruction", 10, 0
m_sys:    db "crashy: asking the kernel to print kernel memory", 10, 0
m_loop:   db "crashy: spinning forever (kill me)", 10, 0
