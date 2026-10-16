BITS 32
%include "rorthox-config.inc"
section .asm
global bad_stack
; Restore ESP from a preserved register, never from the deliberately bad stack.
bad_stack:
    push ebx
    mov eax, [esp+8]
    mov edx, [esp+12]
    mov ebx, esp
    mov esp, edx
    int SYSCALL_VECTOR
    mov esp, ebx
    pop ebx
    ret
