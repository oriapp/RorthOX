#ifndef POTONGOS_POTONGOS_H
#define POTONGOS_POTONGOS_H
#include <stddef.h>
#include <stdbool.h>

// A linked list of arguments
struct command_argument
{
    char argument[512];
    struct command_argument *next;
};

struct process_arguments
{
    int argc;
    char **argv;
};

void print(const char *message);
int potongos_getkey();
void potongos_putchar(char c);

void *potongos_malloc(size_t size);
void *potongos_free(void *ptr);

int potongos_getkeyblock();
struct command_argument *potongos_parse_command(const char *command, int max);
void potongos_terminal_readline(char *out, int max, bool output_while_typing);


void potongos_process_load_start(const char *filename);

int potongos_system(struct command_argument *arguments);
int potongos_system_run(const char *command);

void potongos_process_get_arguments(struct process_arguments *arguments);

void potongos_exit();

#endif // !POTONGOS_POTONGOS_H