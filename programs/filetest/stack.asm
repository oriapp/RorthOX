BITS 32
%include "rorthox-config.inc"
section .asm
global bad_stack
; int bad_stack(int command, unsigned int sp): a syscall with ESP set to sp, so its stack
; operands are whatever lies at sp. ESP comes back from a preserved register.
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
