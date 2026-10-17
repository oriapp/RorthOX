#ifndef RORTHOX_RORTHOX_H
#define RORTHOX_RORTHOX_H
#include <stddef.h>
#include <stdbool.h>
#include "rorthox-config.h"

// A linked list of arguments
struct command_argument
{
    char argument[RORTHOX_COMMAND_ARGUMENT_SIZE];
    struct command_argument *next;
};

struct process_arguments
{
    int argc;
    char **argv;
};

void print(const char *message);
void printc(const char* str, size_t color);
// Returns 0 when no key is waiting, a byte (1-255) for an ordinary key, or a RORTHOX_KEY_* value
// (above 255) for the arrows, Home, End, Insert, Delete, Page Up and Page Down. Keep it in an int.
int rorthox_getkey();
void rorthox_putchar(char c);
// Moves the screen cursor by delta cells (negative = back), across rows, without changing the
// text. The screen is shared by every program. Returns 0, or a negative error (cursor unchanged)
// if the new place is off the visible 80x25 page: it never scrolls.
int rorthox_terminal_move_cursor(int delta);

void *rorthox_malloc(size_t size);
void *rorthox_free(void *ptr);

// Blocks until a byte or special key is available; keep the result in an int.
int rorthox_getkeyblock();
struct command_argument *rorthox_parse_command(const char *command, int max);

// Keeps the whole echoed line visible, including after the screen scrolls.
#define RORTHOX_READLINE_ECHO_LIMIT 1023
// Reads at most max-1 bytes, capped at RORTHOX_READLINE_ECHO_LIMIT when echoing. Returns on
// CR or a full buffer, terminated and without a newline, with the cursor at the line's end.
// Left/Right/Home/End move; Delete/Backspace erase; other special keys and LF are ignored.
// No echo means no terminal calls and no extra cap. NULL/max<=0 do nothing; max==1 writes NUL.
// Editing assumes no other program writes to the shared screen during the read.
void rorthox_terminal_readline(char *out, int max, bool output_while_typing);


void rorthox_process_load_start(const char *filename);

int rorthox_system(struct command_argument *arguments);
int rorthox_system_run(const char *command);

void rorthox_process_get_arguments(struct process_arguments *arguments);

void rorthox_exit();

#endif // !RORTHOX_RORTHOX_H