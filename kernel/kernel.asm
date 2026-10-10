%include "rorthox-config.inc"
[BITS 32]   ; All code from here will be 32-bit

global _start
global kernel_registers
extern kernel_main
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

times 512-($ - $$) db 0
