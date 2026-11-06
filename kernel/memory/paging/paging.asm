[BITS 32]

section .asm

global paging_load_directory
global enable_paging

paging_load_directory:
    push ebp
    mov ebp, esp
    mov eax, [ebp+8]
    mov cr3, eax
    pop ebp
    ret

; Sets CR0.PG (bit 31) and CR0.WP (bit 16) together and keeps every other CR0 bit.
; With WP set, ring 0 stores obey the R/W bits too (Intel SDM Vol. 3A 2.5, 4.6.1),
; so every kernel store must go through a writable mapping in the loaded directory.
enable_paging:
    push ebp
    mov ebp, esp
    mov eax, cr0
    or eax, 0x80010000
    mov cr0, eax
    pop ebp
    ret