BITS 16

start:
    mov ax, 0xb800
    mov es, ax
    xor di, di
    mov ah, 0x07
    mov al, 'H'
    stosw
    mov al, 'i'
    stosw
    jmp $

times 510-($-$$) db 0
dw 0xaa55
