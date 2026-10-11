/*
 * Disk read failure probe for scripts/tests/disk-read.sh (only copied into test images).
 *
 * disktest.elf <file>: prints "disktest: armed" and waits for a key, then starts <file> with
 * syscall 7 and prints "disktest: result <n>" (the syscall's return value). Then it prints
 * "disktest: waiting for a key", waits for one more key and prints "disktest: key", so the test
 * can tell that the kernel came back and the keyboard still reaches this program, and ends with
 * "disktest: done". No globals: the loader doesn't map .bss.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"

// One line per print call: the shell runs alongside and may print between two calls.
static void print_result(int result)
{
    char line[64];
    strcpy(line, "disktest: result ");
    strcpy(line + strlen(line), itoa(result));
    strcpy(line + strlen(line), "\n");
    print(line);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        print("disktest: usage disktest.elf <file>\n");
        return 1;
    }

    struct command_argument *command = malloc(sizeof(struct command_argument));
    if (!command)
    {
        print("disktest: no memory\n");
        return 1;
    }
    strncpy(command->argument, argv[1], sizeof(command->argument));
    command->next = 0;

    print("disktest: armed\n");
    rorthox_getkeyblock();
    print_result(rorthox_system(command));

    print("disktest: waiting for a key\n");
    rorthox_getkeyblock();
    print("disktest: key\n");

    free(command);
    print("disktest: done\n");
    return 0;
}
