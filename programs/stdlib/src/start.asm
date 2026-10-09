[BITS 32]

section .asm

global _start
extern c_start
extern rorthox_exit

global _start

_start:
    call c_start
    call rorthox_exit
    ret

;   Debug
;   ndisasm -b32 ./blank.bin