; stage 1 bootloader

bits 16
org 0x7c00

jmp main

; define disk signature and partition table
disk_signature:
    dd 0xdeadbeef
    times 62 db 0
    dw 0xaa55
partition_table:
    times 16 db 0

main:
    mov bx, 0x8000   ; set up stack pointer
    mov ss, bx
    mov sp, 0x0000

    mov si, msg      ; print message to screen
    call print_string

    jmp $

print_string:
    pusha           ; save registers
    mov ah, 0x0e     ; BIOS tty output function
.loop:
    lodsb           ; load next byte from string
    or al, al
    jz .done        ; if end of string, jump to done
    int 0x10        ; otherwise, print character
    jmp .loop
.done:
    popa            ; restore registers
    ret

msg:
    db "Hello, world!", 0

times 510 - ($ - $$) db 0   ; pad with zeros to 510 bytes
dw 0xaa55                   ; add boot signature at end of sector
