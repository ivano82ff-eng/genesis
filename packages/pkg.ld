/* Packages are flat, position-independent binaries: one section from 0. */
OUTPUT_FORMAT(elf64-x86-64)
SECTIONS
{
    . = 0;
    .all :
    {
        *(.text.entry)
        *(.text) *(.text.*)
        *(.rodata) *(.rodata.*)
        *(.data) *(.data.*)
        *(.bss) *(.bss.*) *(COMMON)
        BYTE(0)
    }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
