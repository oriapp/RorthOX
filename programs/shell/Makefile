FILES=./build/shell.o
CONFIG_DIR ?= $(abspath ../../build/config)
INCLUDES= -I ../stdlib/src -I$(CONFIG_DIR)
FLAGS= -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP
all: ./build/shell.elf

./build/shell.elf: ${FILES} ../stdlib/stdlib.elf linker.ld $(CONFIG_DIR)/rorthox-config.ld
	i686-elf-gcc -g -Wl,-L,$(CONFIG_DIR) -T ./linker.ld -o ./build/shell.elf -ffreestanding -O0 -nostdlib -fpic -g ${FILES} ../stdlib/stdlib.elf

./build/shell.o: ./src/shell.c
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./src/shell.c -o ./build/shell.o

.PHONY: all clean

clean:
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./build/shell.elf

-include $(FILES:.o=.d)
