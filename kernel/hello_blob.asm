section .rodata
align 16
global hello_bin
global hello_bin_end
hello_bin:
incbin "build/hello.bin"
hello_bin_end:
