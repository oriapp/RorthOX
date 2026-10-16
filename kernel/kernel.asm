%include "rorthox-config.inc"
[BITS 32]   ; All code from here will be 32-bit

global _start
global kernel_registers
global kernel_boot_init
global multiboot_header
global multiboot_entry
global multiboot_gdt
extern kernel_main
extern __bss_start
extern __bss_end
extern cas
CODE_SEG equ KERNEL_CODE_SELECTOR
DATA_SEG equ KERNEL_DATA_SELECTOR


_start:
    ; Set data registers
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov ebp, KERNEL_BOOT_STACK_TOP
    mov esp, ebp
    ; Set data registers - End

    ; Enable the A20 line
    in al, 0x92
    or al, 2
    out 0x92, al

; The Multiboot entry joins here, with the same segments, stack and IF=0
kernel_boot_init:
    ; Clear .bss before any C code runs. kernel.bin has no bytes for it,
    ; so it holds whatever was in memory after the kernel load
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi        ; byte count, may be odd or zero
    xor eax, eax
    cld                 ; rep stosb counts up only with DF clear
    rep stosb

    ; Remap both PICs to the configured vector bases.
    ; The out to 0x80 after each PIC write is a short delay for real hardware
    mov al, 00010001b   ; ICW1: init, edge, cascade, ICW4 follows
    out 0x20, al
    out 0x80, al
    out 0xA0, al
    out 0x80, al

    mov al, PIC_MASTER_VECTOR_BASE ; ICW2 master
    out 0x21, al
    out 0x80, al
    mov al, PIC_SLAVE_VECTOR_BASE ; ICW2 slave
    out 0xA1, al
    out 0x80, al

    mov al, 00000100b   ; ICW3 master: slave on IR2
    out 0x21, al
    out 0x80, al
    mov al, 0x02        ; ICW3 slave: cascade id 2
    out 0xA1, al
    out 0x80, al

    mov al, 00000001b   ; ICW4: x86 mode, normal EOI
    out 0x21, al
    out 0x80, al
    out 0xA1, al
    out 0x80, al

    ; ICW1 cleared the masks, so set them now
    mov al, 11111000b   ; Master: only timer, keyboard and cascade
    out 0x21, al
    out 0x80, al
    mov al, 11111111b   ; Slave: everything masked
    out 0xA1, al
    out 0x80, al
    ; End remap of the PICs

    ; call cas ; Enable when debung = True
    call kernel_main

    jmp $

kernel_registers:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov gs, ax
    mov fs, ax
    ret

; Second boot path: GRUB (Multiboot 1) loads kernel.bin as a flat binary.
; The header uses the address fields (flags bit 16): load the whole file at
; KERNEL_LOAD_ADDRESS, zero up to __bss_end and enter at multiboot_entry.
; The boot sector still jumps to _start, which never runs the code below.
MULTIBOOT_HEADER_MAGIC equ 0x1BADB002
MULTIBOOT_HEADER_FLAGS equ 0x00010000  ; address fields only, no other requests
MULTIBOOT_BOOT_MAGIC   equ 0x2BADB002  ; in EAX at entry

align 4, db 0
multiboot_header:
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_HEADER_FLAGS
    dd -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS) ; checksum
    dd multiboot_header     ; header_addr
    dd KERNEL_LOAD_ADDRESS  ; load_addr: file offset 0 is _start
    dd 0                    ; load_end_addr: the whole file
    dd __bss_end            ; bss_end_addr
    dd multiboot_entry      ; entry_addr

; GRUB enters in 32-bit protected mode with paging off, IF=0, A20 on,
; flat CS and DS from its own GDT and no usable stack. No segment register
; may be loaded before our own GDT is in place. EBX holds the Multiboot
; information pointer; nothing reads it yet, so it is not kept.
multiboot_entry:
    cli
    cmp eax, MULTIBOOT_BOOT_MAGIC
    jne .bad_magic
    lgdt [multiboot_gdt_descriptor] ; through GRUB's flat DS
    jmp CODE_SEG:.reload
.reload:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov ebp, KERNEL_BOOT_STACK_TOP
    mov esp, ebp
    ; A20 is already on, so skip the port 0x92 write
    jmp kernel_boot_init

.bad_magic:             ; not started by a Multiboot loader
    cli
    hlt
    jmp .bad_magic

; Same layout as the boot sector's GDT: null, flat ring-0 code, flat data
align 8, db 0
multiboot_gdt:
    dq 0
multiboot_gdt_code:
    dw 0xffff           ; limit 0-15
    dw 0                ; base 0-15
    db 0                ; base 16-23
    db 0x9a             ; present, ring 0, code, execute/read
    db 11001111b        ; 4 KiB granularity, 32-bit, limit 16-19
    db 0                ; base 24-31
multiboot_gdt_data:
    dw 0xffff
    dw 0
    db 0
    db 0x92             ; present, ring 0, data, read/write
    db 11001111b
    db 0
multiboot_gdt_end:
; Reject a mismatch with the configured kernel selectors.
times -(((multiboot_gdt_code - multiboot_gdt) != KERNEL_CODE_SELECTOR) || ((multiboot_gdt_data - multiboot_gdt) != KERNEL_DATA_SELECTOR)) db 0

multiboot_gdt_descriptor:
    dw multiboot_gdt_end - multiboot_gdt - 1
    dd multiboot_gdt

; The bootstrap must stay 512 bytes: this fails to assemble if it grows.
times 512-($ - $$) db 0
