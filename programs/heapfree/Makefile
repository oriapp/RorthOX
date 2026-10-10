FILES=./build/heapfree.o ./build/heap.o
CONFIG_DIR ?= $(abspath ../../build/config)
# The kernel directory is for config.h and the kernel heap header.
INCLUDES= -I ../stdlib/src -I$(CONFIG_DIR) -I ../../kernel
# The kernel's own heap.c is built into this program, with the kernel's include paths.
HEAP_INCLUDES= -I$(CONFIG_DIR) -I ../../kernel -I ../../kernel/memory/heap
FLAGS= -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP
all: ./build/heapfree.elf

./build/heapfree.elf: ${FILES} ../stdlib/stdlib.elf linker.ld $(CONFIG_DIR)/rorthox-config.ld
	i686-elf-gcc -g -Wl,-L,$(CONFIG_DIR) -T ./linker.ld -o ./build/heapfree.elf -ffreestanding -O0 -nostdlib -fpic -g ${FILES} ../stdlib/stdlib.elf

./build/heapfree.o: ./heapfree.c $(CONFIG_DIR)/rorthox-config.h
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./heapfree.c -o ./build/heapfree.o

./build/heap.o: ../../kernel/memory/heap/heap.c $(CONFIG_DIR)/rorthox-config.h
	i686-elf-gcc ${HEAP_INCLUDES} $(FLAGS) -std=gnu99 -c ../../kernel/memory/heap/heap.c -o ./build/heap.o

.PHONY: all clean

clean:
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./build/heapfree.elf

-include $(FILES:.o=.d)
