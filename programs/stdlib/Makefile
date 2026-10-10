FILES=./build/start.asm.o ./build/rorthox.asm.o ./build/rorthox.o ./build/stdlib.o ./build/stdio.o ./build/string.o ./build/memory.o ./build/start.o
CONFIG_DIR ?= $(abspath ../../build/config)
INCLUDES= -I ./src -I$(CONFIG_DIR)
FLAGS= -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP
all: ./stdlib.elf

./stdlib.elf: ${FILES}
	i686-elf-ld -m elf_i386 -relocatable ${FILES} -o ./stdlib.elf

./build/start.asm.o: ./src/start.asm
	nasm -f elf ./src/start.asm -o ./build/start.asm.o

./build/rorthox.asm.o: ./src/rorthox.asm $(CONFIG_DIR)/rorthox-config.inc
	nasm -f elf -I$(CONFIG_DIR)/ ./src/rorthox.asm -o ./build/rorthox.asm.o

./build/rorthox.o: ./src/rorthox.c
	i686-elf-gcc ${INCLUDES} ${FLAGS} -std=gnu99 -c ./src/rorthox.c -o ./build/rorthox.o

./build/stdlib.o: ./src/stdlib.c
	i686-elf-gcc ${INCLUDES} ${FLAGS} -std=gnu99 -c ./src/stdlib.c -o ./build/stdlib.o

./build/stdio.o: ./src/stdio.c
	i686-elf-gcc ${INCLUDES} ${FLAGS} -std=gnu99 -c ./src/stdio.c -o ./build/stdio.o

./build/string.o: ./src/string.c
	i686-elf-gcc ${INCLUDES} ${FLAGS} -std=gnu99 -c ./src/string.c -o ./build/string.o

./build/memory.o: ./src/memory.c
	i686-elf-gcc ${INCLUDES} ${FLAGS} -std=gnu99 -c ./src/memory.c -o ./build/memory.o

./build/start.o: ./src/start.c
	i686-elf-gcc ${INCLUDES} ${FLAGS} -std=gnu99 -c ./src/start.c -o ./build/start.o

.PHONY: all clean

clean:
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./stdlib.elf

-include $(FILES:.o=.d)
