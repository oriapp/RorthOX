[BITS 32]   ; All code from here will be 32-bit

global _start
global kernel_registers
extern kernel_main
extern cas
CODE_SEG equ 0x08
DATA_SEG equ 0x10


_start:
    ; Set data registers
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov ebp, 0x00200000
    mov esp, ebp
    ; Set data registers - End

    ; Enable the A20 line
    in al, 0x92
    or al, 2
    out 0x92, al

    ; Remap both PICs: IRQ0-7 -> 0x20-0x27, IRQ8-15 -> 0x28-0x2F
    ; The out to 0x80 after each PIC write is a short delay for real hardware
    mov al, 00010001b   ; ICW1: init, edge, cascade, ICW4 follows
    out 0x20, al
    out 0x80, al
    out 0xA0, al
    out 0x80, al

    mov al, 0x20        ; ICW2 master: vectors 0x20-0x27
    out 0x21, al
    out 0x80, al
    mov al, 0x28        ; ICW2 slave: vectors 0x28-0x2F
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
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov gs, ax
    mov fs, ax
    ret

times 512-($ - $$) db 0
