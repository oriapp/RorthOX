/*
 * Program lookup test for scripts/tests/program-lookup.sh (only copied into the test image).
 *
 * One program with two jobs. The test image has it as 0:/looktest.elf and copies of it in
 * 0:/bin/ under other names:
 *
 *   looktest.elf s6 <name>          start <name> with syscall 6
 *   looktest.elf s7 <name> <tag>    start <name> with syscall 7, arguments <name> <tag>
 *   looktest.elf neg                start names whose root file is broken or missing; print what
 *                                   each call returned, then "looktest: done"
 *   anything else (the copies)      print "looktest: payload <argc> <argv...>" on one line
 *
 * One line per print call: the shell runs alongside and may print between two calls.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "memory.h"

// Syscall 6 leaves its result in eax; the stdlib wrapper is declared void.
int load_start_result(const char *filename) __asm__("rorthox_process_load_start");

static void say(const char *a, const char *b, const char *c)
{
    char line[200];
    strcpy(line, "looktest: ");
    strcpy(line + strlen(line), a);
    strcpy(line + strlen(line), " ");
    strcpy(line + strlen(line), b);
    strcpy(line + strlen(line), " ");
    strcpy(line + strlen(line), c);
    strcpy(line + strlen(line), "\n");
    print(line);
}

static int system_with_tag(const char *name, const char *tag)
{
    struct command_argument command[2];
    memset(command, 0, sizeof(command));
    strncpy(command[0].argument, name, sizeof(command[0].argument));
    strncpy(command[1].argument, tag, sizeof(command[1].argument));
    command[0].next = &command[1];
    return rorthox_system(command);
}

static int payload(int argc, char **argv)
{
    char line[200];
    strcpy(line, "looktest: payload ");
    strcpy(line + strlen(line), itoa(argc));
    for (int i = 0; i < argc && strlen(line) + strlen(argv[i]) + 3 < sizeof(line); i++)
    {
        strcpy(line + strlen(line), " ");
        strcpy(line + strlen(line), argv[i]);
    }
    strcpy(line + strlen(line), "\n");
    print(line);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strncmp(argv[1], "s6", 3))
    {
        rorthox_process_load_start(argv[2]);
        return 0;
    }
    if (argc == 4 && !strncmp(argv[1], "s7", 3))
    {
        system_with_tag(argv[2], argv[3]);
        return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "neg", 4))
    {
        // The first three have a valid copy in 0:/bin/ that must not start; nosuch.elf is nowhere.
        const char *names[] = { "brk.elf", "empty.elf", "big.elf", "nosuch.elf" };
        for (int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        {
            say("s7", names[i], itoa(system_with_tag(names[i], "x")));
        }
        // Syscall 6 last: if one of them did start a program, the lines above are already out.
        say("s6", "brk.elf", itoa(load_start_result("brk.elf")));
        say("s6", "nosuch.elf", itoa(load_start_result("nosuch.elf")));
        print("looktest: done\n");
        return 0;
    }
    return payload(argc, argv);
}
