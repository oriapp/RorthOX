[BITS 32]

section .asm

global _start
extern c_start
extern potongos_exit

global _start

_start:
    call c_start
    call potongos_exit
    ret

;   Debug
;   ndisasm -b32 ./blank.bin