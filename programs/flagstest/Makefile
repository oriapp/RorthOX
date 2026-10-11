FILES=./build/flagstest.o ./build/flagsprobe.asm.o
CONFIG_DIR ?= $(abspath ../../build/config)
# Only the user library headers and the build config are needed.
INCLUDES= -I ../stdlib/src -I$(CONFIG_DIR)
FLAGS= -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP
all: ./build/flagstest.elf

./build/flagstest.elf: ${FILES} ../stdlib/stdlib.elf linker.ld $(CONFIG_DIR)/rorthox-config.ld
	i686-elf-gcc -g -Wl,-L,$(CONFIG_DIR) -T ./linker.ld -o ./build/flagstest.elf -ffreestanding -O0 -nostdlib -fpic -g ${FILES} ../stdlib/stdlib.elf

./build/flagstest.o: ./flagstest.c $(CONFIG_DIR)/rorthox-config.h
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./flagstest.c -o ./build/flagstest.o

./build/flagsprobe.asm.o: ./flagsprobe.asm $(CONFIG_DIR)/rorthox-config.inc
	nasm -f elf -I$(CONFIG_DIR)/ ./flagsprobe.asm -o ./build/flagsprobe.asm.o

.PHONY: all clean

clean:
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./build/flagstest.elf

-include $(FILES:.o=.d)
