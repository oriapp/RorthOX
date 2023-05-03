# Define the toolchain we'll be using
NASM=nasm
QEMU=qemu-system-i386

# Define the sources for each stage
STAGE1_SRC=stage1.asm
STAGE2_SRC=stage2.asm

# Define the output files for each stage
STAGE1_BIN=stage1.bin
STAGE2_BIN=stage2.bin

# Define the linker script for linking stage1 and stage2 together
LINKER_SCRIPT=link.ld

all: bootloader

bootloader: $(STAGE1_BIN) $(STAGE2_BIN)
	cat $(STAGE1_BIN) $(STAGE2_BIN) > bootloader.bin

run: bootloader.bin
	$(QEMU) -drive file=bootloader.bin,format=raw

$(STAGE1_BIN): $(STAGE1_SRC)
	$(NASM) -f bin -o $@ $<

$(STAGE2_BIN): $(STAGE2_SRC)
	$(NASM) -f elf -o stage2.o $<
	ld -T $(LINKER_SCRIPT) -m elf_i386 -o $@ stage2.o

clean:
	rm -f $(STAGE1_BIN) $(STAGE2_BIN) bootloader.bin stage2.o
