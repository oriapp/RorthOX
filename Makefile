include config.mk

# Config values must come from config.mk, not untracked make command-line overrides.
CONFIG_KEYS := RESERVED_SECTORS SECTORS_PER_FAT SECTOR_SIZE KERNEL_LOAD_ADDRESS KERNEL_BOOT_STACK_TOP KERNEL_TSS_STACK_TOP PROGRAM_LOAD_ADDRESS USER_STACK_TOP USER_STACK_SIZE KERNEL_CODE_SELECTOR KERNEL_DATA_SELECTOR USER_CODE_SELECTOR USER_DATA_SELECTOR TSS_SELECTOR SYSCALL_VECTOR PIC_MASTER_VECTOR_BASE PIC_SLAVE_VECTOR_BASE TOTAL_INTERRUPTS SYSCALL_SUM SYSCALL_PRINT SYSCALL_GETKEY SYSCALL_PUTCHAR SYSCALL_MALLOC SYSCALL_FREE SYSCALL_PROCESS_LOAD_START SYSCALL_INVOKE_SYSTEM_COMMAND SYSCALL_GET_PROGRAM_ARGUMENTS SYSCALL_EXIT SYSCALL_PRINTC COMMAND_ARGUMENT_SIZE RORTHOX_NAME RORTHOX_VERSION QEMU_RAM_MIB
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
		'#define RORTHOX_PIC_MASTER_VECTOR_BASE $(PIC_MASTER_VECTOR_BASE)' \
		'#define RORTHOX_PIC_SLAVE_VECTOR_BASE $(PIC_SLAVE_VECTOR_BASE)' \
		'#define RORTHOX_TOTAL_INTERRUPTS $(TOTAL_INTERRUPTS)' \
		'#define SYSCALL_SUM $(SYSCALL_SUM)' \
		'#define SYSCALL_PRINT $(SYSCALL_PRINT)' \
		'#define SYSCALL_GETKEY $(SYSCALL_GETKEY)' \
		'#define SYSCALL_PUTCHAR $(SYSCALL_PUTCHAR)' \
		'#define SYSCALL_MALLOC $(SYSCALL_MALLOC)' \
		'#define SYSCALL_FREE $(SYSCALL_FREE)' \
		'#define SYSCALL_PROCESS_LOAD_START $(SYSCALL_PROCESS_LOAD_START)' \
		'#define SYSCALL_INVOKE_SYSTEM_COMMAND $(SYSCALL_INVOKE_SYSTEM_COMMAND)' \
		'#define SYSCALL_GET_PROGRAM_ARGUMENTS $(SYSCALL_GET_PROGRAM_ARGUMENTS)' \
		'#define SYSCALL_EXIT $(SYSCALL_EXIT)' \
		'#define SYSCALL_PRINTC $(SYSCALL_PRINTC)' \
		'#define RORTHOX_COMMAND_ARGUMENT_SIZE $(COMMAND_ARGUMENT_SIZE)' \
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
		'%define SYSCALL_VECTOR $(SYSCALL_VECTOR)' \
		'%define PIC_MASTER_VECTOR_BASE $(PIC_MASTER_VECTOR_BASE)' \
		'%define PIC_SLAVE_VECTOR_BASE $(PIC_SLAVE_VECTOR_BASE)' \
		'%define TOTAL_INTERRUPTS $(TOTAL_INTERRUPTS)' \
		'%define SYSCALL_SUM $(SYSCALL_SUM)' \
		'%define SYSCALL_PRINT $(SYSCALL_PRINT)' \
		'%define SYSCALL_GETKEY $(SYSCALL_GETKEY)' \
		'%define SYSCALL_PUTCHAR $(SYSCALL_PUTCHAR)' \
		'%define SYSCALL_MALLOC $(SYSCALL_MALLOC)' \
		'%define SYSCALL_FREE $(SYSCALL_FREE)' \
		'%define SYSCALL_PROCESS_LOAD_START $(SYSCALL_PROCESS_LOAD_START)' \
		'%define SYSCALL_INVOKE_SYSTEM_COMMAND $(SYSCALL_INVOKE_SYSTEM_COMMAND)' \
		'%define SYSCALL_GET_PROGRAM_ARGUMENTS $(SYSCALL_GET_PROGRAM_ARGUMENTS)' \
		'%define SYSCALL_EXIT $(SYSCALL_EXIT)' \
		'%define SYSCALL_PRINTC $(SYSCALL_PRINTC)' > $@.tmp
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
		'QEMU_RAM_MIB=$(QEMU_RAM_MIB)' \
		'PIC_MASTER_VECTOR_BASE=$(PIC_MASTER_VECTOR_BASE)' \
		'PIC_SLAVE_VECTOR_BASE=$(PIC_SLAVE_VECTOR_BASE)' > $@.tmp
	@mv $@.tmp $@

NUMERIC_CONFIG_KEYS := $(filter-out RORTHOX_NAME RORTHOX_VERSION,$(CONFIG_KEYS))
ifneq ($(shell printf '%s\n' $(foreach key,$(NUMERIC_CONFIG_KEYS),'$(strip $($(key)))') | LC_ALL=C grep -Ec '^(0x[0-9a-fA-F]+|0|[1-9][0-9]*)$$'),$(words $(NUMERIC_CONFIG_KEYS)))
$(error numeric config.mk values must be hexadecimal (0x...) or decimal without leading zeros)
endif

ifneq ($(shell [ $$(( $(COMMAND_ARGUMENT_SIZE) )) -ge 2 ] && echo valid),valid)
$(error COMMAND_ARGUMENT_SIZE must hold a character and terminator)
endif

SYSCALL_NUMBERS := $(shell for n in $(SYSCALL_SUM) $(SYSCALL_PRINT) $(SYSCALL_GETKEY) $(SYSCALL_PUTCHAR) $(SYSCALL_MALLOC) $(SYSCALL_FREE) $(SYSCALL_PROCESS_LOAD_START) $(SYSCALL_INVOKE_SYSTEM_COMMAND) $(SYSCALL_GET_PROGRAM_ARGUMENTS) $(SYSCALL_EXIT) $(SYSCALL_PRINTC); do echo $$((n)); done)
ifneq ($(words $(sort $(SYSCALL_NUMBERS))),11)
$(error syscall numbers must be distinct)
endif

ifneq ($(shell [ $$(( $(TOTAL_INTERRUPTS) )) -ge 256 ] && \
	[ $$(( $(SYSCALL_VECTOR) )) -lt $$(( $(TOTAL_INTERRUPTS) )) ] && \
	[ $$(( $(PIC_MASTER_VECTOR_BASE) + 8 )) -le $$(( $(TOTAL_INTERRUPTS) )) ] && \
	[ $$(( $(PIC_SLAVE_VECTOR_BASE) + 8 )) -le $$(( $(TOTAL_INTERRUPTS) )) ] && echo valid),valid)
$(error TOTAL_INTERRUPTS must cover all 256 CPU vectors, the syscall and both PIC ranges)
endif

# PIC ICW2 bases are eight-vector aligned, disjoint and outside CPU exceptions.
ifneq ($(shell [ $$(( $(PIC_MASTER_VECTOR_BASE) % 8 )) -eq 0 ] && \
	[ $$(( $(PIC_SLAVE_VECTOR_BASE) % 8 )) -eq 0 ] && \
	[ $$(( $(PIC_MASTER_VECTOR_BASE) )) -ge 32 ] && \
	[ $$(( $(PIC_SLAVE_VECTOR_BASE) )) -ge 32 ] && \
	[ $$(( $(PIC_MASTER_VECTOR_BASE) + 8 )) -le 256 ] && \
	[ $$(( $(PIC_SLAVE_VECTOR_BASE) + 8 )) -le 256 ] && \
	( [ $$(( $(PIC_MASTER_VECTOR_BASE) + 8 )) -le $$(( $(PIC_SLAVE_VECTOR_BASE) )) ] || \
	  [ $$(( $(PIC_SLAVE_VECTOR_BASE) + 8 )) -le $$(( $(PIC_MASTER_VECTOR_BASE) )) ] ) && \
	[ $$(( $(SYSCALL_VECTOR) )) -ge 32 ] && \
	[ $$(( $(SYSCALL_VECTOR) )) -lt 256 ] && \
	( [ $$(( $(SYSCALL_VECTOR) )) -lt $$(( $(PIC_MASTER_VECTOR_BASE) )) ] || \
	  [ $$(( $(SYSCALL_VECTOR) )) -ge $$(( $(PIC_MASTER_VECTOR_BASE) + 8 )) ] ) && \
	( [ $$(( $(SYSCALL_VECTOR) )) -lt $$(( $(PIC_SLAVE_VECTOR_BASE) )) ] || \
	  [ $$(( $(SYSCALL_VECTOR) )) -ge $$(( $(PIC_SLAVE_VECTOR_BASE) + 8 )) ] ) && echo valid),valid)
$(error PIC bases must be aligned, outside exceptions, within 256 and disjoint; SYSCALL_VECTOR must be 32..255 and clear of both ranges)
endif

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

./build/idt/idt.asm.o: ./kernel/idt/idt.asm $(CONFIG_INC)
	nasm -f elf -g -Ibuild/config/ ./kernel/idt/idt.asm -o ./build/idt/idt.asm.o

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

# Test programs (heaptest.elf, heapfree.elf, oomtest.elf, pathtest.elf, printoom.elf, tasktest.elf, flagstest.elf) go only into the test images that scripts/tests/ build, not into IMAGE_FILES.
user_programs: | $(CONFIG_H) $(CONFIG_INC) $(CONFIG_LD) $(CONFIG_ENV)
	cd ./programs/stdlib && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/blank && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/shell && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/heaptest && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/heapfree && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/oomtest && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/pathtest && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/printoom && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/tasktest && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all
	cd ./programs/flagstest && $(MAKE) CONFIG_DIR="$(CONFIG_DIR)" all

user_programs_clean:
	cd ./programs/stdlib && $(MAKE) clean
	cd ./programs/blank && $(MAKE) clean
	cd ./programs/shell && $(MAKE) clean
	cd ./programs/heaptest && $(MAKE) clean
	cd ./programs/heapfree && $(MAKE) clean
	cd ./programs/oomtest && $(MAKE) clean
	cd ./programs/pathtest && $(MAKE) clean
	cd ./programs/printoom && $(MAKE) clean
	cd ./programs/tasktest && $(MAKE) clean
	cd ./programs/flagstest && $(MAKE) clean

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
