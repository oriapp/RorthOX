ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
SECTIONS
{
    . = 0x400000; /* User Programs (Program virtual address) */
    .text : ALIGN(4096) /* Alinged by 16 bytes */
    {
        *(.text)
    }

    .asm : ALIGN(4096) /* Alinged by 16 bytes */
    {
        *(.asm)
    }

    .rodata : ALIGN(4096) /* Alinged by 16 bytes */
    {
        *(.rodata)
    }

    .data : ALIGN(4096) /* Alinged by 16 bytes */
    {
        *(.data)
    }

    .bss : ALIGN(4096) /* Alinged by 16 bytes */
    {
        *(COMMON)
        *(.bss)
    }
}