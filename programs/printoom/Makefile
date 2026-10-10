FILES=./build/printoom.o
CONFIG_DIR ?= $(abspath ../../build/config)
# The kernel directory is for config.h (the heap geometry) and status.h (the error codes).
INCLUDES= -I ../stdlib/src -I$(CONFIG_DIR) -I ../../kernel
FLAGS= -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP
all: ./build/printoom.elf

./build/printoom.elf: ${FILES} ../stdlib/stdlib.elf linker.ld $(CONFIG_DIR)/rorthox-config.ld
	i686-elf-gcc -g -Wl,-L,$(CONFIG_DIR) -T ./linker.ld -o ./build/printoom.elf -ffreestanding -O0 -nostdlib -fpic -g ${FILES} ../stdlib/stdlib.elf

./build/printoom.o: ./printoom.c $(CONFIG_DIR)/rorthox-config.h
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./printoom.c -o ./build/printoom.o

.PHONY: all clean

clean:
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./build/printoom.elf

-include $(FILES:.o=.d)
