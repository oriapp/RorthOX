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

/* The boot sector jumps to KERNEL_LOAD_ADDRESS, and the Multiboot header's
   load_addr says file offset 0 is loaded there. The header must be 32-bit
   aligned and lie entirely within the first 8192 bytes of the file. */
ASSERT(_start == KERNEL_LOAD_ADDRESS, "_start must be at KERNEL_LOAD_ADDRESS")
ASSERT(multiboot_header % 4 == 0, "Multiboot header must be 4-byte aligned")
ASSERT(multiboot_header >= KERNEL_LOAD_ADDRESS && multiboot_header + 32 <= KERNEL_LOAD_ADDRESS + 8192, "Multiboot header must be in the first 8192 bytes")
