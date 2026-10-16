%include "rorthox-config.inc"
[BITS 32]

section .asm

global flags_after_timer:function
global flags_after_syscall:function
global wait_for_debugger:function

; EFLAGS bits a ring-3 program can set with popfd and that must come back unchanged:
; CF, PF, AF, ZF, SF, DF, OF, AC and ID.
TESTED_FLAGS equ 0x00240cd5

; void wait_for_debugger(void)
; Spins until the test's debugger, stopped at .spin, sets EAX to "frag". This lets the debugger
; put its breakpoints in only after the key has reached the program: QEMU drops key presses that
; arrive while a debugger has the guest stopped.
wait_for_debugger:
    xor eax, eax
.spin:
    cmp eax, 0x67617266
    jne .spin
    ret

; uint32_t flags_after_timer(uint32_t flags, uint32_t *before)
; Sets the tested flags to <flags> and waits in a loop for a real timer interrupt (ring 3 can't
; raise the timer vector with "int": its gate is DPL 0), which saves this task, switches to the
; next one and later comes back through task_return. Returns EFLAGS as they were right after the
; wait. *before gets EFLAGS as they were just before it. "loop" changes no flags.
flags_after_timer:
    push ebp
    mov ebp, esp
    pushfd
    pop ecx
    and ecx, ~TESTED_FLAGS
    mov edx, [ebp+8]
    and edx, TESTED_FLAGS
    or ecx, edx
    push ecx
    popfd
    pushfd                      ; what popfd really set
    pop edx
    mov ecx, 0x2000000          ; many timer ticks long under QEMU; mov changes no flags
.wait_tick:
    loop .wait_tick
    pushfd                      ; before anything can change a flag
    cld
    pop eax
    mov ecx, [ebp+12]
    mov [ecx], edx
    push dword 0x202            ; back to plain flags for the C code
    popfd
    pop ebp
    ret

; uint32_t flags_after_syscall(uint32_t flags, uint32_t *before)
; The same around a syscall (sum of two zeroes), which returns through the CPU's own frame.
flags_after_syscall:
    push ebp
    mov ebp, esp
    push dword 0
    push dword 0
    pushfd
    pop ecx
    and ecx, ~TESTED_FLAGS
    mov edx, [ebp+8]
    and edx, TESTED_FLAGS
    or ecx, edx
    mov eax, SYSCALL_SUM
    push ecx
    popfd
    pushfd
    pop edx
    int SYSCALL_VECTOR
    pushfd
    cld
    pop eax
    add esp, 8
    mov ecx, [ebp+12]
    mov [ecx], edx
    push dword 0x202
    popfd
    pop ebp
    ret
