# FAT16 layout (sectors):
#   sector 0                       boot sector with the BPB
#   sectors 1 .. RESERVED - 1      kernel.bin (KERNEL_SECTORS)
#   RESERVED ..                    FAT 1, FAT 2 (SECTORS_PER_FAT each), root directory, data
# The kernel size guard prevents truncation by the bootloader.
RESERVED_SECTORS = 200
SECTORS_PER_FAT = 256

# Logical disk sector (bytes; only 512 is supported)
SECTOR_SIZE = 512

# Physical kernel location and independent stacks (bytes)
KERNEL_LOAD_ADDRESS = 0x100000
KERNEL_BOOT_STACK_TOP = 0x200000
KERNEL_TSS_STACK_TOP = 0x600000

# User virtual addresses and stack extent (bytes)
PROGRAM_LOAD_ADDRESS = 0x400000
USER_STACK_TOP = 0x3FF000
USER_STACK_SIZE = 16384

# Selectors follow the GDT slot order in kernel/kernel.c.
# GDT selectors and syscall interrupt vector
KERNEL_CODE_SELECTOR = 0x08
KERNEL_DATA_SELECTOR = 0x10
USER_CODE_SELECTOR = 0x1b
USER_DATA_SELECTOR = 0x23
TSS_SELECTOR = 0x28
SYSCALL_VECTOR = 0x80
PIC_MASTER_VECTOR_BASE = 0x20
PIC_SLAVE_VECTOR_BASE = 0x28
TOTAL_INTERRUPTS = 512

# Kernel/user syscall ABI numbers
SYSCALL_SUM = 0
SYSCALL_PRINT = 1
SYSCALL_GETKEY = 2
SYSCALL_PUTCHAR = 3
SYSCALL_MALLOC = 4
SYSCALL_FREE = 5
SYSCALL_PROCESS_LOAD_START = 6
SYSCALL_INVOKE_SYSTEM_COMMAND = 7
SYSCALL_GET_PROGRAM_ARGUMENTS = 8
SYSCALL_EXIT = 9
SYSCALL_PRINTC = 10

# Display identity and guest RAM (MiB)
RORTHOX_NAME = RorthOX
RORTHOX_VERSION = 1.0.0
QEMU_RAM_MIB = 128
