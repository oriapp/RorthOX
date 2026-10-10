include config.mk

# Config values must come from config.mk, not untracked make command-line overrides.
CONFIG_KEYS := RESERVED_SECTORS SECTORS_PER_FAT SECTOR_SIZE KERNEL_LOAD_ADDRESS KERNEL_BOOT_STACK_TOP KERNEL_TSS_STACK_TOP PROGRAM_LOAD_ADDRESS USER_STACK_TOP USER_STACK_SIZE KERNEL_CODE_SELECTOR KERNEL_DATA_SELECTOR USER_CODE_SELECTOR USER_DATA_SELECTOR TSS_SELECTOR SYSCALL_VECTOR RORTHOX_NAME RORTHOX_VERSION QEMU_RAM_MIB
$(foreach key,$(CONFIG_KEYS),$(if $(filter command line,$(origin $(key))),$(error $(key) must be set in config.mk, not on the command line)))

FILES = ./build/kernel.asm.o ./build/arch/sys.o ./build/arch/pit.o ./build/arch/sys.asm.o ./build/kernel.o ./build/arch/tty.o ./build/idt/idt.asm.o ./build/idt/idt.o ./build/memory/memory.o ./build/io/io.asm.o ./build/loader/formats/elfloader.o ./build/loader/formats/elf.o ./build/gdt/gdt.o ./build/gdt/gdt.asm.o ./build/memory/heap/heap.o ./build/memory/heap/kheap.o ./build/memory/paging/paging.o ./build/memory/paging/paging.asm.o ./build/isr80h/isr80h.o ./build/isr80h/heap.o ./build/keyboard/keyboard.o ./build/keyboard/classic.o ./build/isr80h/io.o ./build/isr80h/process.o ./build/isr80h/misc.o ./build/disk/disk.o ./build/disk/streamer.o ./build/task/process.o ./build/task/task.o ./build/task/task.asm.o ./build/task/tss.asm.o ./build/fs/pparser.o ./build/fs/file.o ./build/fs/fat/fat16.o ./build/string/string.o
INCLUDES = -I./kernel -Ibuild/config
# -MMD -MP: let the compiler record header dependencies (*.d) so editing a header rebuilds what uses it.
FLAGS = -g -ffreestanding -falign-jumps -falign-functions -falign-labels -falign-loops -fstrength-reduce -fomit-frame-pointer -finline-functions -Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter -nostdlib -nostartfiles -nodefaultlibs -Wall -O0 -Iinc -MMD -MP

# The boot sector occupies the first reserved sector.
KERNEL_SECTORS := $(shell echo $$(($(RESERVED_SECTORS) - 1)))
FAT1_SECTOR := $(RESERVED_SECTORS)
FAT2_SECTOR := $(shell echo $$(($(RESERVED_SECTORS) + $(SECTORS_PER_FAT))))

# Files copied into the FAT16 filesystem of the disk image (with mtools, no root needed).
IMAGE_FILES = ./programs/shell/build/shell.elf ./programs/blank/build/blank.elf

CONFIG_DIR := $(abspath build/config)
CONFIG_H := build/config/rorthox-config.h
CONFIG_INC := build/config/rorthox-config.inc
CONFIG_LD := build/config/rorthox-config.ld
CONFIG_ENV := build/config/rorthox-config.env

# Each rule writes one file in build/config, one value per line.
$(CONFIG_H): config.mk Makefile
	@mkdir -p build/config
	@printf '%s\n' \
		'#ifndef RORTHOX_BUILD_CONFIG_H' \
		'#define RORTHOX_BUILD_CONFIG_H' \
		'#define RORTHOX_SECTOR_SIZE $(SECTOR_SIZE)' \
		'#define RORTHOX_PROGRAM_VIRTUAL_ADDRESS $(PROGRAM_LOAD_ADDRESS)' \
		'#define USER_PROGRAM_STACK_SIZE $(USER_STACK_SIZE)' \
		'#define RORTHOX_PROGRAM_VIRTUAL_STACK_ADDRESS_START $(USER_STACK_TOP)' \
		'#define RORTHOX_KERNEL_TSS_STACK_TOP $(KERNEL_TSS_STACK_TOP)' \
		'#define KERNEL_CODE_SELECTOR $(KERNEL_CODE_SELECTOR)' \
		'#define KERNEL_DATA_SELECTOR $(KERNEL_DATA_SELECTOR)' \
		'#define USER_CODE_SEGMENT $(USER_CODE_SELECTOR)' \
		'#define USER_DATA_SEGMENT $(USER_DATA_SELECTOR)' \
		'#define RORTHOX_TSS_SELECTOR $(TSS_SELECTOR)' \
		'#define RORTHOX_SYSCALL_VECTOR $(SYSCALL_VECTOR)' \
		'#define RORTHOX_NAME "$(RORTHOX_NAME)"' \
		'#define RORTHOX_VERSION "$(RORTHOX_VERSION)"' \
		'#endif' > $@.tmp
	@mv $@.tmp $@

$(CONFIG_INC): config.mk Makefile
	@mkdir -p build/config
	@printf '%s\n' \
		'%define RESERVED_SECTORS $(RESERVED_SECTORS)' \
		'%define SECTORS_PER_FAT $(SECTORS_PER_FAT)' \
		'%define KERNEL_SECTORS $(KERNEL_SECTORS)' \
		'%define SECTOR_SIZE $(SECTOR_SIZE)' \
		'%define KERNEL_LOAD_ADDRESS $(KERNEL_LOAD_ADDRESS)' \
		'%define KERNEL_BOOT_STACK_TOP $(KERNEL_BOOT_STACK_TOP)' \
		'%define KERNEL_CODE_SELECTOR $(KERNEL_CODE_SELECTOR)' \
		'%define KERNEL_DATA_SELECTOR $(KERNEL_DATA_SELECTOR)' \
		'%define USER_DATA_SELECTOR $(USER_DATA_SELECTOR)' \
		'%define SYSCALL_VECTOR $(SYSCALL_VECTOR)' > $@.tmp
	@mv $@.tmp $@

$(CONFIG_LD): config.mk Makefile
	@mkdir -p build/config
	@printf '%s\n' \
		'KERNEL_LOAD_ADDRESS = $(KERNEL_LOAD_ADDRESS);' \
		'PROGRAM_LOAD_ADDRESS = $(PROGRAM_LOAD_ADDRESS);' > $@.tmp
	@mv $@.tmp $@

$(CONFIG_ENV): config.mk Makefile
	@mkdir -p build/config
	@printf '%s\n' \
		'RORTHOX_NAME="$(RORTHOX_NAME)"' \
		'RORTHOX_VERSION="$(RORTHOX_VERSION)"' \
		'QEMU_RAM_MIB=$(QEMU_RAM_MIB)' > $@.tmp
	@mv $@.tmp $@

ifneq ($(SECTOR_SIZE),512)
$(error SECTOR_SIZE must be 512: the BPB and ATA driver support only 512-byte sectors)
endif
# Identity values are emitted into C strings and shell assignments.
ifneq ($(shell printf '%s\n' '$(RORTHOX_NAME)' '$(RORTHOX_VERSION)' | LC_ALL=C grep -Ec '^[A-Za-z0-9][A-Za-z0-9._-]*$$'),2)
$(error RORTHOX_NAME and RORTHOX_VERSION must use ASCII letters, digits, dots, underscores or hyphens)
endif

$(filter-out %.asm.o,$(FILES)): | $(CONFIG_H)

all: ./bin/os.bin

./bin/os.bin: ./bin/boot.bin ./bin/kernel.bin $(IMAGE_FILES) | user_programs
	rm -f $@
	cat ./bin/boot.bin ./bin/kernel.bin > $@
	# Append 16 MiB of zeroes: the FAT tables, root directory and data area of the filesystem.
	dd if=/dev/zero bs=1048576 count=16 status=none >> $@
	# Mark FAT entries 0 and 1 (media byte 0xF8, end-of-chain) in both FAT copies.
	printf '\370\377\377\377' | dd of=$@ bs=$(SECTOR_SIZE) seek=$(FAT1_SECTOR) conv=notrunc status=none
	printf '\370\377\377\377' | dd of=$@ bs=$(SECTOR_SIZE) seek=$(FAT2_SECTOR) conv=notrunc status=none
	mcopy -i $@ $(IMAGE_FILES) ::
	mdir -i $@ ::

$(IMAGE_FILES): | user_programs

./bin/kernel.bin: $(FILES) ./kernel/linker.ld $(CONFIG_LD) config.mk
	i686-elf-ld -g -relocatable $(FILES) -o ./build/kernelfull.o
	i686-elf-gcc $(FLAGS) -Wl,-Map,./build/kernel.map -Wl,-L,$(CONFIG_DIR) -T ./kernel/linker.ld -o ./bin/kernel.bin -ffreestanding -O0 -nostdlib ./build/kernelfull.o
	@size=$$(wc -c < $@); max=$$(($(KERNEL_SECTORS) * $(SECTOR_SIZE))); \
	if [ $$size -gt $$max ]; then \
		echo "error: kernel.bin is $$size bytes but the bootloader loads only $$max ($(KERNEL_SECTORS) sectors)"; \
		rm -f $@; exit 1; \
	fi; echo "kernel.bin: $$size of $$max bytes"

./bin/boot.bin: ./kernel/boot/boot.asm $(CONFIG_INC)
	nasm -f bin -Ibuild/config/ ./kernel/boot/boot.asm -o ./bin/boot.bin

./build/kernel.asm.o: ./kernel/kernel.asm $(CONFIG_INC)
	nasm -f elf -g -Ibuild/config/ ./kernel/kernel.asm -o ./build/kernel.asm.o

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

./build/task/task.asm.o: ./kernel/task/task.asm $(CONFIG_INC)
	nasm -f elf -g -Ibuild/config/ ./kernel/task/task.asm -o ./build/task/task.asm.o

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

user_programs: | $(CONFIG_H) $(CONFIG_INC) $(CONFIG_LD) $(CONFIG_ENV)
	cd ./programs/stdlib && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/blank && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/shell && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all

user_programs_clean:
	cd ./programs/stdlib && $(MAKE) clean
	cd ./programs/blank && $(MAKE) clean
	cd ./programs/shell && $(MAKE) clean

# Boot the image headless in QEMU, type a command into the shell and check the output.
smoke: all
	./scripts/smoke.sh

# The smoke test plus every test in scripts/tests/ (each boots the OS in QEMU).
test: all
	./scripts/run-tests.sh

clean: user_programs_clean
	rm -rf ./bin/boot.bin
	rm -rf ./bin/kernel.bin
	rm -rf ./bin/os.bin
	rm -rf ${FILES} $(FILES:.o=.d)
	rm -rf ./build/kernelfull.o ./build/kernel.map ./build/config ./build/smoke ./build/tests

.PHONY: all user_programs user_programs_clean smoke test clean

-include $(FILES:.o=.d)
