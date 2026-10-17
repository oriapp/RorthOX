%include "rorthox-config.inc"
[BITS 32]

section .asm

global print:function
global rorthox_getkey:function
global rorthox_malloc:function
global rorthox_free:function
global rorthox_putchar:function
global rorthox_process_load_start:function
global rorthox_process_get_arguments:function
global rorthox_system:function
global rorthox_exit:function
global printc:function
global rorthox_terminal_move_cursor:function
global sleep:function

; void print(const char message);
print:
    push ebp
    mov ebp, esp
    push dword[ebp+8]
    mov eax, SYSCALL_PRINT ; Command print
    int SYSCALL_VECTOR
    add esp, 4

    pop ebp
    ret

; void printc(const char* str, size_t color);
printc:
    push ebp
    mov ebp, esp
    push dword[ebp+12] ; Variable "color"
    push dword[ebp+8] ; Variable "str"
    mov eax, SYSCALL_PRINTC ; Command printc
    int SYSCALL_VECTOR
    add esp, 8

    pop ebp
    ret

; int rorthox_getkey();
rorthox_getkey:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_GETKEY ; Command getkey
    int SYSCALL_VECTOR
    pop ebp
    ret

; void rorthox_putchar(char c);
rorthox_putchar:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_PUTCHAR ; Command putchar
    push dword[ebp+8] ; Variable "c"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; int rorthox_terminal_move_cursor(int delta);
rorthox_terminal_move_cursor:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_TERMINAL_MOVE_CURSOR ; Command move cursor, result in eax
    push dword[ebp+8] ; Variable "delta"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; int sleep(int milliseconds);
sleep:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_SLEEP ; Command sleep, result in eax
    push dword[ebp+8] ; Variable "milliseconds"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; void *rorthox_malloc(size_t size);
rorthox_malloc:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_MALLOC ; Command malloc (Allocate memory for the process)
    push dword[ebp+8] ; Variable "size"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; void *rorthox_free(void *ptr);
rorthox_free:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_FREE ; Command free (Frees the allocated memory for this process)
    push dword[ebp+8] ; Variable "ptr"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; void rorthox_process_load_start(const char *filename)
rorthox_process_load_start:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_PROCESS_LOAD_START ; Command process load start ( starts a process )
    push dword[ebp+8] ; Variable "filename"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; int rorthox_system(struct command_argument *arguments);
rorthox_system:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_INVOKE_SYSTEM_COMMAND ; Command process_system (Runs a system command based on the arguments)
    push dword[ebp+8] ; Variable "arguments"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; void rorthox_process_get_arguments(struct process_arguments *arguments)
rorthox_process_get_arguments:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_GET_PROGRAM_ARGUMENTS ; Gets the process arguments
    push dword[ebp+8] ; Variable "arguments"
    int SYSCALL_VECTOR
    add esp, 4
    pop ebp
    ret

; void rorthox_exit();
rorthox_exit:
    push ebp
    mov ebp, esp
    mov eax, SYSCALL_EXIT ; Command process exit
    int SYSCALL_VECTOR
    pop ebp
    ret
