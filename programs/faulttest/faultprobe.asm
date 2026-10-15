[BITS 32]

section .asm

global fault_ud2:function
global fault_int3:function
global fault_cli:function
global fault_read:function
global fault_int14:function
global fault_int18:function
global fault_in:function
global fault_out:function
global fault_in_pic:function
global fault_out_pic:function
global fault_in_high:function

; Each routine is one instruction that ends the program with a CPU exception.
; If the kernel ever resumes the program after it, the routine returns and
; faulttest prints "faulttest: survived".

; void fault_ud2(void): #UD (vector 6), no error code
fault_ud2:
    ud2
    ret

; void fault_int3(void): #BP (vector 3), a trap: the saved EIP is after the int3
fault_int3:
    int3
    ret

; void fault_cli(void): #GP (vector 13) with error code 0, ring 3 runs with IOPL 0
fault_cli:
    cli
    ret

; uint32_t fault_read(void *ptr): reads *ptr. The pointer stays in EAX, so a page
; fault leaves CR2 equal to EAX
fault_read:
    mov eax, [esp+4]
    mov eax, [eax]
    ret

; void fault_int14(void): software "int 14". Gate 14 has DPL 0, so ring 3 gets
; #GP with error code 14*8+2 = 0x72 instead of entering the page fault handler
fault_int14:
    int 14
    ret

; void fault_int18(void): software "int 18". Gate 18 (#MC) keeps DPL 3 and CR4.MCE is clear,
; so only a software int can reach it. From ring 3 it must end the program, not halt the kernel
fault_int18:
    int 18
    ret

; void fault_in(void): reads port 0x80. Ring 3 runs with IOPL 0 and the TSS has no I/O
; permission bitmap, so #GP (vector 13) with error code 0
fault_in:
    in al, 0x80
    ret

; void fault_out(void): writes port 0x80, #GP like fault_in
fault_out:
    xor eax, eax
    out 0x80, al
    ret

; The next three use ports whose bitmap bits would sit inside the TSS (0x20, 0x21) or at the far
; end of a full bitmap (0xffff) if the I/O map base or the TSS limit were wrong. Each must #GP
; like fault_in. If one ever runs, it does no harm: it reads the PIC's interrupt mask, selects the
; IRR for the next read of port 0x20 (the PIC's default), or reads a port with no device.

; void fault_in_pic(void): reads port 0x21 (master PIC interrupt mask)
fault_in_pic:
    in al, 0x21
    ret

; void fault_out_pic(void): writes 0x0a (OCW3: read the IRR) to port 0x20
fault_out_pic:
    mov al, 0x0a
    out 0x20, al
    ret

; void fault_in_high(void): reads port 0xffff through DX, which stays 0xffff at the fault
fault_in_high:
    mov edx, 0xffff
    in al, dx
    ret
