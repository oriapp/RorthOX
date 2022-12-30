ENTRY(_start)
OUTPUT_FORMAT(binary)
SECTIONS
{   
    /* 
    ALIGN(4096) is aligment by the sections themselves
    Specify the alignment of each section within the executable image.
    */
    . = 1M; /* 1MB in memory (1025 * 1024 = into address 100000) */
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