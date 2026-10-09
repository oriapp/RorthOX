FILES = ./build/kernel.asm.o ./build/arch/sys.o ./build/arch/pit.o ./build/arch/sys.asm.o ./build/kernel.o ./build/arch/tty.o ./build/idt/idt.asm.o ./build/idt/idt.o ./build/memory/memory.o ./build/io/io.asm.o ./build/loader/formats/elfloader.o ./build/loader/formats/elf.o ./build/gdt/gdt.o ./build/gdt/gdt.asm.o ./build/memory/heap/heap.o ./build/memory/heap/kheap.o ./build/memory/paging/paging.o ./build/memory/paging/paging.asm.o ./build/isr80h/isr80h.o ./build/isr80h/heap.o ./build/keyboard/keyboard.o ./build/keyboard/classic.o ./build/isr80h/io.o ./build/isr80h/process.o ./build/isr80h/misc.o ./build/disk/disk.o ./build/disk/streamer.o ./build/task/process.o ./build/task/task.o ./build/task/task.asm.o ./build/task/tss.asm.o ./build/fs/pparser.o ./build/fs/file.o ./build/fs/fat/fat16.o ./build/string/string.o
INCLUDES = -I./kernel
# -MMD -MP: let the compiler record header dependencies (*.d) so editing a header rebuilds what uses it.
FLAGS = -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP

# FAT16 disk layout (512-byte sectors). This is the only place these numbers are written down:
# boot.asm gets them for its BPB (nasm -D), and the image and the size guard below use them too.
#   sector 0                       boot sector with the BPB
#   sectors 1 .. RESERVED - 1      kernel.bin, loaded by the boot sector (KERNEL_SECTORS)
#   RESERVED ..                    FAT 1, then FAT 2 (SECTORS_PER_FAT each), root directory, data
# kernel.bin is checked against KERNEL_SECTORS so it can never be silently truncated.
RESERVED_SECTORS = 200
SECTORS_PER_FAT = 256
KERNEL_SECTORS := $(shell echo $$(($(RESERVED_SECTORS) - 1)))
FAT1_SECTOR := $(RESERVED_SECTORS)
FAT2_SECTOR := $(shell echo $$(($(RESERVED_SECTORS) + $(SECTORS_PER_FAT))))

# Files copied into the FAT16 filesystem of the disk image (with mtools, no root needed).
IMAGE_FILES = ./programs/shell/build/shell.elf ./programs/blank/build/blank.elf

OS = $(shell uname -s)

ifeq ($(OS),Linux)
	detected_OS = Linux
endif
ifeq ($(OS),Darwin)
	detected_OS = MacOS
endif
ifeq ($(OS),Windows)
	detected_OS = Windows
endif

all: ./bin/boot.bin ./bin/kernel.bin user_programs
	@echo "Operating System Compiling On: $(detected_OS)"
	rm -f ./bin/os.bin
	cat ./bin/boot.bin ./bin/kernel.bin > ./bin/os.bin
	# Append 16 MiB of zeroes: the FAT tables, root directory and data area of the filesystem.
	dd if=/dev/zero bs=1048576 count=16 status=none >> ./bin/os.bin
	# Mark FAT entries 0 and 1 (media byte 0xF8, end-of-chain) in both FAT copies
	# so tools see a valid, empty filesystem.
	printf '\370\377\377\377' | dd of=./bin/os.bin bs=512 seek=$(FAT1_SECTOR) conv=notrunc status=none
	printf '\370\377\377\377' | dd of=./bin/os.bin bs=512 seek=$(FAT2_SECTOR) conv=notrunc status=none
	mcopy -i ./bin/os.bin $(IMAGE_FILES) ::
	mdir -i ./bin/os.bin ::

./bin/kernel.bin: $(FILES)
	i686-elf-ld -g -relocatable $(FILES) -o ./build/kernelfull.o
	i686-elf-gcc $(FLAGS) -Wl,-Map,./build/kernel.map -T ./kernel/linker.ld -o ./bin/kernel.bin -ffreestanding -O0 -nostdlib ./build/kernelfull.o
	@size=$$(wc -c < $@); max=$$(($(KERNEL_SECTORS) * 512)); \
	if [ $$size -gt $$max ]; then \
		echo "error: kernel.bin is $$size bytes but the bootloader loads only $$max ($(KERNEL_SECTORS) sectors)"; \
		rm -f $@; exit 1; \
	fi; echo "kernel.bin: $$size of $$max bytes"

./bin/boot.bin: ./kernel/boot/boot.asm Makefile
	nasm -f bin -DRESERVED_SECTORS=$(RESERVED_SECTORS) -DSECTORS_PER_FAT=$(SECTORS_PER_FAT) ./kernel/boot/boot.asm -o ./bin/boot.bin

./build/kernel.asm.o: ./kernel/kernel.asm
	nasm -f elf -g ./kernel/kernel.asm -o ./build/kernel.asm.o

./build/kernel.o: ./kernel/kernel.c
	i686-elf-gcc $(INCLUDES) $(FLAGS) -std=gnu99 -c ./kernel/kernel.c -o ./build/kernel.o

./build/arch/tty.o: ./kernel/arch/i686/tty.c
	i686-elf-gcc $(INCLUDES) $(FLAGS) -std=gnu99 -c ./kernel/arch/i686/tty.c -o ./build/arch/tty.o

./build/arch/sys.o: ./kernel/arch/i686/sys.c
	i686-elf-gcc $(INCLUDES) $(FLAGS) -std=gnu99 -c ./kernel/arch/i686/sys.c -o ./build/arch/sys.o

./build/arch/pit.o: ./kernel/arch/i686/pit.c
	i686-elf-gcc $(INCLUDES) $(FLAGS) -std=gnu99 -c ./kernel/arch/i686/pit.c -o ./build/arch/pit.o

./build/arch/sys.asm.o: ./kernel/arch/i686/sys.asm
	nasm -f elf -g ./kernel/arch/i686/sys.asm -o ./build/arch/sys.asm.o

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

# Boot the image headless in QEMU, type a command into the shell and check the output.
smoke: all
	./scripts/smoke.sh

clean: user_programs_clean
	rm -rf ./bin/boot.bin
	rm -rf ./bin/kernel.bin
	rm -rf ./bin/os.bin
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./build/kernelfull.o ./build/kernel.map ./build/smoke

.PHONY: all user_programs user_programs_clean smoke clean

-include $(FILES:.o=.d)
