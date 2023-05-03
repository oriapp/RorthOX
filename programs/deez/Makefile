FILES=./build/deez.o
INCLUDES= -I ../stdlib/src
FLAGS= -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc
all: ${FILES}
	i686-elf-gcc -g -T ./linker.ld -o ./build/deez.elf -ffreestanding -O0 -nostdlib -fpic -g ${FILES} ../stdlib/stdlib.elf

./build/deez.o: ./src/deez.c
	i686-elf-gcc ${INCLUDES} -I./ $(FLAGS) -std=gnu99 -c ./src/deez.c -o ./build/deez.o

clean:
	rm -rf ${FILES}
	rm -rf ./build/deez.elf