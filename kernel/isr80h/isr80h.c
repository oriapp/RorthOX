#include "isr80h.h"
#include "idt/idt.h"
#include "misc.h"
#include "io.h"
#include "heap.h"
#include "process.h"
#include "file.h"

void isr80h_register_commands()
{
    isr80h_register_command(SYSTEM_COMMAND0_SUM, isr80h_command0_sum);
    isr80h_register_command(SYSTEM_COMMAND1_PRINT, isr80h_command1_print);
    isr80h_register_command(SYSTEM_COMMAND2_GETKEY, isr80h_command2_getkey);
    isr80h_register_command(SYSTEM_COMMAND3_PUTCHAR, isr80h_command3_putchar);
    isr80h_register_command(SYSTEM_COMMAND4_MALLOC, isr80h_command4_malloc);
    isr80h_register_command(SYSTEM_COMMAND5_FREE, isr80h_command5_free);
    isr80h_register_command(SYSTEM_COMMAND6_PROCESS_LOAD_START, isr80h_command6_process_load_start);
    isr80h_register_command(SYSTEM_COMMAND7_INVOKE_SYSTEM_COMMAND, isr80h_command7_invoke_system_command);
    isr80h_register_command(SYSTEM_COMMAND8_GET_PROGRAM_ARGUMENTS, isr80h_command8_get_program_arguments);
    isr80h_register_command(SYSTEM_COMMAND9_EXIT, isr80h_command9_exit);
    isr80h_register_command(SYSTEM_COMMAND10_PRINTC, isr80h_command11_printc);
    isr80h_register_command(SYSTEM_COMMAND11_TERMINAL_MOVE_CURSOR, isr80h_command_terminal_move_cursor);
    isr80h_register_command(SYSTEM_COMMAND12_SLEEP, isr80h_command_sleep);
    isr80h_register_command(SYSTEM_COMMAND13_FILE_OPEN, isr80h_command_file_open);
    isr80h_register_command(SYSTEM_COMMAND14_FILE_READ, isr80h_command_file_read);
    isr80h_register_command(SYSTEM_COMMAND15_FILE_WRITE, isr80h_command_file_write);
    isr80h_register_command(SYSTEM_COMMAND16_FILE_SEEK, isr80h_command_file_seek);
    isr80h_register_command(SYSTEM_COMMAND17_FILE_STAT, isr80h_command_file_stat);
    isr80h_register_command(SYSTEM_COMMAND18_FILE_CLOSE, isr80h_command_file_close);
    isr80h_register_command(SYSTEM_COMMAND19_GET_TICKS, isr80h_command19_get_ticks);
}
