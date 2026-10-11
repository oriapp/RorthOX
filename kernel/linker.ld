ENTRY(_start)
OUTPUT_FORMAT(binary)
INCLUDE rorthox-config.ld
SECTIONS
{   
    /* 
    ALIGN(4096) is aligment by the sections themselves
    Specify the alignment of each section within the executable image.
    */
    . = KERNEL_LOAD_ADDRESS;
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

    /* _start clears [__bss_start, __bss_end) before any C code runs */
    .bss : ALIGN(4096) /* Aligned to 4096 bytes */
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }
}