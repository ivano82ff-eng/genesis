section .rodata
global mbr_bin
global mbr_bin_end
mbr_bin:
incbin "build/mbr.bin"
mbr_bin_end:
