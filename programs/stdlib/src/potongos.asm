[BITS 32]

section .asm

global print:function
global potongos_getkey:function
global potongos_malloc:function
global potongos_free:function
global potongos_putchar:function
global potongos_process_load_start:function
global potongos_process_get_arguments:function
global potongos_system:function
global potongos_exit:function

; void print(const char message);
print:
    push ebp
    mov ebp, esp
    push dword[ebp+8]
    mov eax, 1 ; Command print
    int 0x80
    add esp, 4

    pop ebp
    ret

; int potongos_getkey();
potongos_getkey:
    push ebp
    mov ebp, esp
    mov eax, 2 ; Command getkey
    int 0x80
    pop ebp
    ret

; void potongos_putchar(char c);
potongos_putchar:
    push ebp
    mov ebp, esp
    mov eax, 3 ; Command 3 putchar
    push dword[ebp+8] ; Variable "c"
    int 0x80
    add esp, 4
    pop ebp
    ret

; void *potongos_malloc(size_t size);
potongos_malloc:
    push ebp
    mov ebp, esp
    mov eax, 4 ; Command malloc (Allocate memory for the process)
    push dword[ebp+8] ; Variable "size"
    int 0x80
    add esp, 4
    pop ebp
    ret

; void *potongos_free(void *ptr);
potongos_free:
    push ebp
    mov ebp, esp
    mov eax, 5 ; Command free (Frees the allocated memory for this process)
    push dword[ebp+8] ; Variable "ptr"
    int 0x80
    add esp, 4
    pop ebp
    ret

; void potongos_process_load_start(const char *filename)
potongos_process_load_start:
    push ebp
    mov ebp, esp
    mov eax, 6 ; Command 6 process load start ( starts a process )
    push dword[ebp+8] ; Variable "filename"
    int 0x80
    add esp, 4
    pop ebp
    ret

; int potongos_system(struct command_argument *arguments);
potongos_system:
    push ebp
    mov ebp, esp
    mov eax, 7 ; Command 7 process_system (Runs a system command based on the arguments)
    push dword[ebp+8] ; Variable "arguments"
    int 0x80
    add esp, 4
    pop ebp
    ret

; void potongos_process_get_arguments(struct process_arguments *arguments)
potongos_process_get_arguments:
    push ebp
    mov ebp, esp
    mov eax, 8 ; Command 8 Gets the process arguments
    push dword[ebp+8] ; Variable "arguments"
    int 0x80
    add esp, 4
    pop ebp
    ret

; void potongos_exit();
potongos_exit:
    push ebp
    mov ebp, esp
    mov eax, 9 ; Command process exit
    int 0x80
    pop ebp
    ret
