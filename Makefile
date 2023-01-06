FILES = ./build/kernel.asm.o ./build/kernel.o ./build/stdio/io.o ./build/idt/idt.asm.o ./build/idt/idt.o ./build/memory/memory.o ./build/io/io.asm.o ./build/loader/formats/elfloader.o ./build/loader/formats/elf.o ./build/gdt/gdt.o ./build/gdt/gdt.asm.o ./build/memory/heap/heap.o ./build/memory/heap/kheap.o ./build/memory/paging/paging.o ./build/memory/paging/paging.asm.o ./build/isr80h/isr80h.o ./build/isr80h/heap.o ./build/keyboard/keyboard.o ./build/keyboard/classic.o ./build/isr80h/io.o ./build/isr80h/process.o ./build/isr80h/misc.o ./build/disk/disk.o ./build/disk/streamer.o ./build/task/process.o ./build/task/task.o ./build/task/task.asm.o ./build/task/tss.asm.o ./build/fs/pparser.o ./build/fs/file.o ./build/fs/fat/fat16.o ./build/string/string.o
INCLUDES = -I./kernel
FLAGS = -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc

all: ./bin/boot.bin ./bin/kernel.bin user_programs
	cd libc && $(MAKE)
	rm -rf ./bin/os.bin
	dd if=./bin/boot.bin >> ./bin/os.bin
	dd if=./bin/kernel.bin >> ./bin/os.bin
	dd if=/dev/zero bs=1048576 count=16 >> ./bin/os.bin
	# 1048576 = 1 MB 
	# count=16 is 16 MB of null

	sudo mount -t vfat ./bin/os.bin ./bin/mnt/d
	# Copy a file over
	sudo cp ./hello.txt ./bin/mnt/d

	# Copy files over
	sudo cp ./programs/blank/build/blank.elf ./bin/mnt/d
	sudo cp ./programs/shell/build/shell.elf ./bin/mnt/d

	sudo umount ./bin/mnt/d
	# Remeber each sector size is 512!

./bin/kernel.bin: $(FILES)
	i686-elf-ld -g -relocatable $(FILES) -o ./build/kernelfull.o
	i686-elf-gcc $(FLAGS) -T ./kernel/linker.ld -o ./bin/kernel.bin -ffreestanding -O0 -nostdlib ./build/kernelfull.o

./bin/boot.bin: ./kernel/boot/boot.asm
	nasm -f bin ./kernel/boot/boot.asm -o ./bin/boot.bin

./build/kernel.asm.o: ./kernel/kernel.asm
	nasm -f elf -g ./kernel/kernel.asm -o ./build/kernel.asm.o

./build/kernel.o: ./kernel/kernel.c
	i686-elf-gcc $(INCLUDES) $(FLAGS) -std=gnu99 -c ./kernel/kernel.c -o ./build/kernel.o

./build/stdio/io.o: ./kernel/stdio/io.c
	i686-elf-gcc $(INCLUDES) $(FLAGS) -std=gnu99 -c ./kernel/stdio/io.c -o ./build/stdio/io.o

./build/idt/idt.asm.o: ./kernel/idt/idt.asm
	nasm -f elf -g ./kernel/idt/idt.asm -o ./build/idt/idt.asm.o

./build/loader/formats/elf.o: ./kernel/loader/formats/elf.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/loader/formats $(FLAGS) -std=gnu99 -c ./kernel/loader/formats/elf.c -o ./build/loader/formats/elf.o

./build/loader/formats/elfloader.o: ./kernel/loader/formats/elfloader.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/loader/formats $(FLAGS) -std=gnu99 -c ./kernel/loader/formats/elfloader.c -o ./build/loader/formats/elfloader.o

./build/gdt/gdt.o: ./kernel/gdt/gdt.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/gdt $(FLAGS) -std=gnu99 -c ./kernel/gdt/gdt.c -o ./build/gdt/gdt.o

./build/gdt/gdt.asm.o: ./kernel/gdt/gdt.asm
	nasm -f elf -g ./kernel/gdt/gdt.asm -o ./build/gdt/gdt.asm.o

./build/isr80h/isr80h.o: ./kernel/isr80h/isr80h.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/isr80h $(FLAGS) -std=gnu99 -c ./kernel/isr80h/isr80h.c -o ./build/isr80h/isr80h.o

./build/isr80h/heap.o: ./kernel/isr80h/heap.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/isr80h $(FLAGS) -std=gnu99 -c ./kernel/isr80h/heap.c -o ./build/isr80h/heap.o

./build/isr80h/misc.o: ./kernel/isr80h/misc.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/isr80h $(FLAGS) -std=gnu99 -c ./kernel/isr80h/misc.c -o ./build/isr80h/misc.o

./build/isr80h/io.o: ./kernel/isr80h/io.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/isr80h $(FLAGS) -std=gnu99 -c ./kernel/isr80h/io.c -o ./build/isr80h/io.o

./build/isr80h/process.o: ./kernel/isr80h/process.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/isr80h $(FLAGS) -std=gnu99 -c ./kernel/isr80h/process.c -o ./build/isr80h/process.o

./build/keyboard/keyboard.o: ./kernel/keyboard/keyboard.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/keyboard $(FLAGS) -std=gnu99 -c ./kernel/keyboard/keyboard.c -o ./build/keyboard/keyboard.o

./build/keyboard/classic.o: ./kernel/keyboard/classic.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/keyboard $(FLAGS) -std=gnu99 -c ./kernel/keyboard/classic.c -o ./build/keyboard/classic.o

./build/idt/idt.o: ./kernel/idt/idt.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/idt $(FLAGS) -std=gnu99 -c ./kernel/idt/idt.c -o ./build/idt/idt.o

./build/memory/memory.o: ./kernel/memory/memory.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/memory $(FLAGS) -std=gnu99 -c ./kernel/memory/memory.c -o ./build/memory/memory.o

./build/task/process.o: ./kernel/task/process.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/task $(FLAGS) -std=gnu99 -c ./kernel/task/process.c -o ./build/task/process.o

./build/task/task.o: ./kernel/task/task.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/task $(FLAGS) -std=gnu99 -c ./kernel/task/task.c -o ./build/task/task.o

./build/task/tss.asm.o: ./kernel/task/tss.asm
	nasm -f elf -g ./kernel/task/tss.asm -o ./build/task/tss.asm.o

./build/task/task.asm.o: ./kernel/task/task.asm
	nasm -f elf -g ./kernel/task/task.asm -o ./build/task/task.asm.o

./build/io/io.asm.o: ./kernel/io/io.asm
	nasm -f elf -g ./kernel/io/io.asm -o ./build/io/io.asm.o

./build/memory/heap/heap.o: ./kernel/memory/heap/heap.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/memory/heap $(FLAGS) -std=gnu99 -c ./kernel/memory/heap/heap.c -o ./build/memory/heap/heap.o

./build/memory/heap/kheap.o: ./kernel/memory/heap/kheap.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/memory/kheap $(FLAGS) -std=gnu99 -c ./kernel/memory/heap/kheap.c -o ./build/memory/heap/kheap.o

./build/memory/paging/paging.o: ./kernel/memory/paging/paging.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/memory/paging $(FLAGS) -std=gnu99 -c ./kernel/memory/paging/paging.c -o ./build/memory/paging/paging.o

./build/memory/paging/paging.asm.o: ./kernel/memory/paging/paging.asm
	nasm -f elf -g ./kernel/memory/paging/paging.asm -o ./build/memory/paging/paging.asm.o

./build/disk/disk.o: ./kernel/disk/disk.c
		i686-elf-gcc $(INCLUDES) -I ./kernel/disk $(FLAGS) -std=gnu99 -c ./kernel/disk/disk.c -o ./build/disk/disk.o

./build/disk/streamer.o: ./kernel/disk/streamer.c
		i686-elf-gcc $(INCLUDES) -I ./kernel/disk $(FLAGS) -std=gnu99 -c ./kernel/disk/streamer.c -o ./build/disk/streamer.o

./build/fs/fat/fat16.o: ./kernel/fs/fat/fat16.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/fs -I ./kernel/fat $(FLAGS) -std=gnu99 -c ./kernel/fs/fat/fat16.c -o ./build/fs/fat/fat16.o

./build/fs/file.o: ./kernel/fs/file.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/fs $(FLAGS) -std=gnu99 -c ./kernel/fs/file.c -o ./build/fs/file.o

./build/fs/pparser.o: ./kernel/fs/pparser.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/fs $(FLAGS) -std=gnu99 -c ./kernel/fs/pparser.c -o ./build/fs/pparser.o

./build/string/string.o: ./kernel/string/string.c
	i686-elf-gcc $(INCLUDES) -I ./kernel/string $(FLAGS) -std=gnu99 -c ./kernel/string/string.c -o ./build/string/string.o

user_programs:
	cd ./programs/stdlib && $(MAKE) all
	cd ./programs/blank && $(MAKE) all
	cd ./programs/shell && $(MAKE) all

user_programs_clean:
	cd ./programs/stdlib && $(MAKE) clean
	cd ./programs/blank && $(MAKE) clean
	cd ./programs/shell && $(MAKE) clean

clean: user_programs_clean
	rm -rf ./bin/boot.bin
	rm -rf ./bin/kernel.bin
	rm -rf ./bin/os.bin
	rm -rf ${FILES}
	rm -rf ./build/kernelfull.o
	cd libc && $(MAKE) clean