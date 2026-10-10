OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_load_start = .;

    .text ALIGN(8) :
    {
        *(.text)
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

    kernel_image_end = .;

    .bss ALIGN(4096) :
    {
        bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        bss_end = .;
    }

    kernel_load_end = .;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.*)
        *(.comment)
        *(.gnu.build.attributes)
    }
}
