[BITS 32]

section .asm

global fpu_has_cpuid
global fpu_cpuid
global fpu_read_cr0
global fpu_write_cr0
global fpu_read_cr4
global fpu_write_cr4
global fpu_save
global fpu_restore

; The kernel's only x87/SSE instructions are the FXSAVE and FXRSTOR below. Neither waits for a
; pending x87 exception (Intel SDM Vol. 2A FXSAVE/FXRSTOR: no #MF), so a task's unmasked
; exception is only raised at its own next waiting FP instruction, in ring 3.

; int fpu_has_cpuid(void)
; 1 if software can flip EFLAGS.ID (bit 21): then the CPU has CPUID (Intel SDM Vol. 2A, CPUID).
; Puts the original EFLAGS back, so IF and the other flags don't change.
fpu_has_cpuid:
    pushfd                          ; original EFLAGS, popped last
    pushfd
    xor dword [esp], 0x00200000     ; flip ID
    popfd
    pushfd
    pop eax                         ; EFLAGS as the CPU kept them
    xor eax, [esp]                  ; the bits that changed
    popfd
    shr eax, 21
    and eax, 1
    ret

; void fpu_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *edx)
; CPUID also writes EBX, which the caller expects to keep.
fpu_cpuid:
    push ebx
    mov eax, [esp+8]
    xor ecx, ecx                    ; sub-leaf 0
    cpuid
    mov ecx, [esp+12]
    mov [ecx], eax
    mov ecx, [esp+16]
    mov [ecx], edx
    pop ebx
    ret

; uint32_t fpu_read_cr0(void)
fpu_read_cr0:
    mov eax, cr0
    ret

; void fpu_write_cr0(uint32_t value)
fpu_write_cr0:
    mov eax, [esp+4]
    mov cr0, eax
    ret

; uint32_t fpu_read_cr4(void)
; Only after the CPUID check: CPUs without CPUID have no CR4.
fpu_read_cr4:
    mov eax, cr4
    ret

; void fpu_write_cr4(uint32_t value)
fpu_write_cr4:
    mov eax, [esp+4]
    mov cr4, eax
    ret

; void fpu_save(void *area)
; Stores x87, MMX, XMM0-7 and MXCSR (CR4.OSFXSR is set) in the 512-byte legacy format and
; leaves the registers as they are.
fpu_save:
    mov eax, [esp+4]
    fxsave [eax]
    ret

; void fpu_restore(void *area)
fpu_restore:
    mov eax, [esp+4]
    fxrstor [eax]
    ret
