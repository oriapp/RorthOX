# FAT16 layout (sectors):
#   sector 0                       boot sector with the BPB
#   sectors 1 .. RESERVED - 1      kernel.bin (KERNEL_SECTORS)
#   RESERVED ..                    FAT 1, FAT 2 (SECTORS_PER_FAT each), root directory, data
# The kernel size guard prevents truncation by the bootloader.
RESERVED_SECTORS = 200
SECTORS_PER_FAT = 256

# Logical disk sector (bytes; only 512 is supported)
SECTOR_SIZE = 512
