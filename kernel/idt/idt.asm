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
    mov ebx, [ebp+8] ; ebp = stack pointer
                     ; ebp+4 = return addressof the function caller
                     ; ebp+8 = first argument passed to this function, that we're creating
    lidt [ebx]
    ; Our function

    pop ebp
    ret


no_interrupt:
    pushad
    call no_interrupt_handler
    popad
    iret 

%macro interrupt 1
    global int%1
    int%1:
        ; INTERRUPT FRAME START
        ; ALREDY PUSHED TO US BY THE PROCESSOR UPON ENTRY TO THIS INTERRUPT
        ; uint32_t ip;
        ; uint32_t cs;
        ; uint32_t flags;
        ; uint32_t sp; // stack pointer
        ; uint32_t ss; // stack segment
        ; Pushes the general purpose registers to the stack
        pushad
        ; Interrupt frame end
        push esp
        push dword %1
        call interrupt_handler
        add esp, 8 ; pop the two added values
        popad
        iret
%endmacro

%assign i 0
%rep 512 ; loop 512
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
    ; Pushes the general purpose registers to the stack
    pushad

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
    iretd

section .data
    ; Inside here is stored the return result from isr80h_handler
    tmp_res: dd 0


%macro interrupt_array_entry 1
    dd int%1
%endmacro

interrupt_pointer_table:
%assign i 0
%rep 512
    interrupt_array_entry i
%assign i i+1
%endrep

; Resources
; pushad - https://faydoc.tripod.com/cpu/pushad.htm