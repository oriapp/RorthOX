[BITS 32]

section .asm

global _start
extern main

global _start

_start:
    call main
    ret

;   Debug
;   ndisasm -b32 ./blank.bin