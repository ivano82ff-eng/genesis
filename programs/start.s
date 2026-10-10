bits 64
section .text
global _start
extern viewer_main

_start:
    jmp viewer_main
