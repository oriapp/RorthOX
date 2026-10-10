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

    .bss : ALIGN(4096) /* Aligned to 4096 bytes */
    {
        *(COMMON)
        *(.bss)
    }
}