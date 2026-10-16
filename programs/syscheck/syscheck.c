#include "rorthox.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "memory.h"

/* Observe EAX without changing the public void wrapper. */
int get_arguments(struct process_arguments *) __asm__("rorthox_process_get_arguments");

static void armed(void)
{
    print("syscheck: armed\n");
    rorthox_getkeyblock();
}

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    struct command_argument *node = malloc(sizeof(*node));
    if (!node) return 1;
    memset(node, 0, sizeof(*node));
    strcpy(node->argument, argv[1]);
    armed();
    int result = rorthox_system(node);
    printf("syscheck: result %i\n", result);
    print("syscheck: continued\n");
    free(node);
    return 0;
}
