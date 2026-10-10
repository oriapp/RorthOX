ENTRY(_start)
OUTPUT_FORMAT(elf32-i386)
INCLUDE rorthox-config.ld
SECTIONS
{
    . = PROGRAM_LOAD_ADDRESS; /* User Programs (Program virtual address) */
    .text : ALIGN(4096) /* Aligned to 4096 bytes */
    {
        *(.text)
    }

    .asm : ALIGN(4096) /* Aligned to 4096 bytes */
    {
        *(.asm)
    }

    .rodata : ALIGN(4096) /* Aligned to 4096 bytes */
    {
        *(.rodata)
    }

    .data : ALIGN(4096) /* Aligned to 4096 bytes */
    {
        *(.data)
    }

    .bss : ALIGN(4096) /* Aligned to 4096 bytes */
    {
        *(COMMON)
        *(.bss)
    }
}