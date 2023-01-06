section .asm

global IRQ_OFF
global IRQ_RES
global PAUSE
global IRQ_ON_AND_PAUSE

IRQ_OFF:       ; Turn off IRQ
    cli

IRQ_RES:
    sti

PAUSE:       ; Halt the system
    hlt

IRQ_ON_AND_PAUSE:
    sti
    hlt
    cli
