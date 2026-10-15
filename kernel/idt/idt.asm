%include "rorthox-config.inc"
%if TOTAL_INTERRUPTS != 256
%fatal "TOTAL_INTERRUPTS must be 256: x86 has 256 interrupt vectors"
%endif
section .asm

extern int21h_handler
extern no_interrupt_handler
extern isr80h_handler
extern interrupt_handler

global idt_load
global no_interrupt
global enable_interrupts
global disable_interrupts
global isr80h_wrapper
global interrupt_pointer_table

enable_interrupts:
    sti
    ret

disable_interrupts:
    cli
    ret

idt_load:
    push ebp
    mov ebp, esp

    ; Our function
    mov eax, [ebp+8] ; ebp = stack pointer
                     ; ebp+4 = return addressof the function caller
                     ; ebp+8 = first argument passed to this function, that we're creating
    lidt [eax]       ; eax, not ebx: cdecl callers expect ebx to be preserved
    ; Our function

    pop ebp
    ret


no_interrupt:
    pushad
    call no_interrupt_handler
    popad
    iret 

; Vectors where the CPU pushes an error code after EIP (Intel SDM Vol 3A, Table 6-1:
; #DF 8, #TS 10, #NP 11, #SS 12, #GP 13, #PF 14, #AC 17, #CP 21; AMD APM Vol 2: #VC 29, #SX 30).
; A software int never pushes one, so idt.c gives these gates DPL 0.
%define error_code_vector(n) ((n) == 8 || ((n) >= 10 && (n) <= 14) || (n) == 17 || (n) == 21 || (n) == 29 || (n) == 30)

%macro interrupt 1
    global int%1
    int%1:
        ; INTERRUPT FRAME START
        ; ALREDY PUSHED TO US BY THE PROCESSOR UPON ENTRY TO THIS INTERRUPT
        ; uint32_t ip;
        ; uint32_t cs;
        ; uint32_t flags;
        ; uint32_t sp; // stack pointer, only when coming from ring 3
        ; uint32_t ss; // stack segment, only when coming from ring 3
%if error_code_vector(%1)
        ; the CPU pushed the error code below ip
%else
        push dword 0 ; no CPU error code: push 0 so every frame looks the same
%endif
        ; Pushes the general purpose registers to the stack
        pushad
        cld ; the C code expects DF clear, and the interrupt gate doesn't clear it
        ; Interrupt frame end
        mov ebx, esp ; the frame
        mov eax, cr2 ; read before any C code, a later page fault would change it
%if %1 == 2
        ; An NMI can come in while the kernel runs with other segments loaded, and the
        ; handler returns to it. Keep its segments outside the frame.
        push ds
        push es
        push fs
        push gs
%endif
        push eax
        push ebx
        push dword %1
        call interrupt_handler
        add esp, 12 ; pop the three added values
%if %1 == 2
        pop gs
        pop fs
        pop es
        pop ds
%endif
        popad
        add esp, 4 ; drop the error code
        iretd
%endmacro

%assign i 0
%rep TOTAL_INTERRUPTS
    interrupt i
%assign i i+1
%endrep

isr80h_wrapper:
    ; INTERRUPT FRAME START
    ; ALREDY PUSHED TO US BY THE PROCESSOR UPON ENTRY TO THIS INTERRUPT
    ; uint32_t ip;
    ; uint32_t cs;
    ; uint32_t flags;
    ; uint32_t sp; // stack pointer
    ; uint32_t ss; // stack segment
    push dword 0 ; no error code, same frame as the interrupt stubs
    ; Pushes the general purpose registers to the stack
    pushad
    cld ; the C code expects DF clear, and the interrupt gate doesn't clear it

    ; INTERRUPT FRAME ENDS
    
    ; Push the stack pointer so that we are pointing to the interrupt frame
    push esp

    ; EAX holds our command lets push it to the stack for isr80h_handler
    push eax
    call isr80h_handler
    mov dword[tmp_res], eax
    add esp, 8 ; each word is 4 bytes
    ; pop ebx
    ; pop ebx

    ; Restore general purpose registers for user land
    popad ; Pops all pushad
    ; Mov the result into the eax register
    mov eax, [tmp_res]
    add esp, 4 ; drop the error code
    iretd

section .data
    ; Inside here is stored the return result from isr80h_handler
    tmp_res: dd 0


%macro interrupt_array_entry 1
    dd int%1
%endmacro

interrupt_pointer_table:
%assign i 0
%rep TOTAL_INTERRUPTS
    interrupt_array_entry i
%assign i i+1
%endrep

; Resources
; pushad - https://faydoc.tripod.com/cpu/pushad.htm