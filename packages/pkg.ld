/* Packages are flat, position-independent binaries: one section from 0. */
SECTIONS
{
    . = 0;
    .all :
    {
        *(.text.entry)
        *(.text) *(.text.*)
        *(.rodata) *(.rodata.*)
        *(.data) *(.data.*)
        *(.got) *(.got.plt)
        *(.bss) *(.bss.*) *(COMMON)
        BYTE(0)
    }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
