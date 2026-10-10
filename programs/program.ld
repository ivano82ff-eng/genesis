OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x600000;

    .text ALIGN(16) :
    {
        *start.o(.text)
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(16) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(16) :
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(16) :
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
}
