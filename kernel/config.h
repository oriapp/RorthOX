#ifndef CONFIG_H
#define CONFIG_H

#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10


#define POTONGOS_TOTAL_INTERRUPTS 512

// 100MB heap size
#define POTONGOS_HEAP_SIZE_BYTES 104857600
// 1024*1024 = 1048576*100 = 104857600 (100 MB)
#define POTONGOS_HEAP_BLOCK_SIZE 4096
#define POTONGOS_HEAP_ADDRESS 0x01000000
#define POTONGOS_HEAP_TABLE_ADDRESS 0x00007E00

#define POTONGOS_SECTOR_SIZE 512

#define POTONGOS_MAX_FILESYSTEMS 12
#define POTONGOS_MAX_FILE_DESCRIPTORS 512

#define POTONGOS_MAX_PATH 108

#define POTONGOS_TOTAL_GDT_SEGMENTS 6 // null segment, kernel code, and kernel data

#define POTONGOS_PROGRAM_VIRTUAL_ADDRESS 0x400000
#define USER_PROGRAM_STACK_SIZE 1024 * 16 // 16Kb stack
#define POTONGOS_PROGRAM_VIRTUAL_STACK_ADDRESS_START 0x3FF000
#define POTONGOS_PROGRAM_VIRTUAL_STACK_ADDRESS_END POTONGOS_PROGRAM_VIRTUAL_STACK_ADDRESS_START - USER_PROGRAM_STACK_SIZE

#define POTONGOS_MAX_PROGRAM_ALLOCATIONS 1024
#define POTONGOS_MAX_PROCESSES 12

#define USER_DATA_SEGMENT 0x23
#define USER_CODE_SEGMENT 0x1b

#define POTONGOS_MAX_ISR80H_COMMANDS 1024

#define POTONGOS_KEYBOARD_BUFFER_SIZE 1024
#endif