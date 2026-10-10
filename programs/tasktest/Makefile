FILES=./build/tasktest.o
CONFIG_DIR ?= $(abspath ../../build/config)
# Only the user library headers and the build config are needed.
INCLUDES= -I ../stdlib/src -I$(CONFIG_DIR)
FLAGS= -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP
all: ./build/tasktest.elf

./build/tasktest.elf: ${FILES} ../stdlib/stdlib.elf linker.ld $(CONFIG_DIR)/rorthox-config.ld
	i686-elf-gcc -g -Wl,-L,$(CONFIG_DIR) -T ./linker.ld -o ./build/tasktest.elf -ffreestanding -O0 -nostdlib -fpic -g ${FILES} ../stdlib/stdlib.elf

./build/tasktest.o: ./tasktest.c $(CONFIG_DIR)/rorthox-config.h
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./tasktest.c -o ./build/tasktest.o

.PHONY: all clean

clean:
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./build/tasktest.elf

-include $(FILES:.o=.d)
