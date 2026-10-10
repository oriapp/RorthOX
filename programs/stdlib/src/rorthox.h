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
int rorthox_getkey();
void rorthox_putchar(char c);

void *rorthox_malloc(size_t size);
void *rorthox_free(void *ptr);

int rorthox_getkeyblock();
struct command_argument *rorthox_parse_command(const char *command, int max);
void rorthox_terminal_readline(char *out, int max, bool output_while_typing);


void rorthox_process_load_start(const char *filename);

int rorthox_system(struct command_argument *arguments);
int rorthox_system_run(const char *command);

void rorthox_process_get_arguments(struct process_arguments *arguments);

void rorthox_exit();

#endif // !RORTHOX_RORTHOX_H