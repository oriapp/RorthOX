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
