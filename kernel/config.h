#ifndef CONFIG_H
#define CONFIG_H
#include "rorthox-config.h"


#define DEBUG true  // Debug Features
#define IODBG false // System I/O Debug

// 100MB heap size
#define RORTHOX_HEAP_SIZE_BYTES 104857600
// 1024*1024 = 1048576*100 = 104857600 (100 MB)
#define RORTHOX_HEAP_BLOCK_SIZE 4096
#define RORTHOX_HEAP_ADDRESS 0x01000000
#define RORTHOX_HEAP_TABLE_ADDRESS 0x00007E00

_Static_assert(PROCESS_HEAP_LIMIT_BYTES > 0 && PROCESS_HEAP_LIMIT_BYTES <= RORTHOX_HEAP_SIZE_BYTES &&
               PROCESS_HEAP_LIMIT_BYTES % RORTHOX_HEAP_BLOCK_SIZE == 0,
               "process heap limit must be a positive page multiple within the kernel heap");

_Static_assert(ELF_FILE_LIMIT_BYTES > 0 && ELF_FILE_LIMIT_BYTES <= RORTHOX_HEAP_SIZE_BYTES &&
               ELF_FILE_LIMIT_BYTES % RORTHOX_HEAP_BLOCK_SIZE == 0,
               "ELF file limit must be a positive page multiple within the kernel heap");


#define RORTHOX_MAX_FILESYSTEMS 12
#define RORTHOX_MAX_FILE_DESCRIPTORS 512

#define RORTHOX_MAX_PATH 108

#define RORTHOX_TOTAL_GDT_SEGMENTS 6 // null segment, kernel code, and kernel data

#define RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_END RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_START - USER_PROGRAM_STACK_SIZE

#define RORTHOX_MAX_PROGRAM_ALLOCATIONS 1024
#define RORTHOX_MAX_PROCESSES 12


#define RORTHOX_MAX_ISR80H_COMMANDS 1024

#define RORTHOX_KEYBOARD_BUFFER_SIZE 1024
#endif