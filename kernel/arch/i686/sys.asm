section .asm

global IRQ_OFF
global IRQ_RES
global PAUSE
global IRQ_ON_AND_PAUSE

IRQ_OFF:       ; Turn off IRQ
    cli
    ret

IRQ_RES:       ; Turn IRQ back on
    sti
    ret

PAUSE:       ; Halt the system for good (never returns)
    cli
.halt:
    hlt         ; an NMI can still wake the CPU up, so halt again
    jmp .halt

IRQ_ON_AND_PAUSE:  ; Enable IRQ and wait for the next interrupt
    sti
    hlt
    cli
    ret
