section .asm

global tss_load

; Loads in the task switch segment
tss_load:
    push ebp
    mov ebp, esp
    mov ax, [ebp+8] ; TSS Segment (provided to us)
    ltr ax          ; Load Task Register
    pop ebp
    ret

; Load Task Register - https://www.felixcloutier.com/x86/ltr
; https://wiki.osdev.org/Task_State_Segment