/*
 * Allocation budget test for scripts/tests/process-limit.sh (only copied into the test image).
 *
 * Every program may have PROCESS_HEAP_LIMIT_BYTES of malloc memory, charged in whole 4 KiB pages.
 * Its argument array and argument strings (one page each) count too, and so do the command nodes
 * it makes for syscall 7.
 *
 * "budget.elf" (the parent) makes the command for its child first, then works out what is
 * left of its budget and checks that one byte more is refused, the rest fits, one more byte is
 * refused, a freed allocation can be allocated again, a 1-byte allocation takes a whole page,
 * a second free and a free inside an allocation give nothing back, and freeing the 1-byte
 * allocation gives its whole page back. Then, still holding its whole budget, it starts
 * "budget.elf child" with syscall 7 and sleeps for ever (it stays alive with its budget used up).
 *
 * "budget.elf child": while the parent holds its whole budget, the rest of the child's own
 * budget fits and one more byte is refused. Then it ends.
 *
 * Each program prints "<who>: FAIL <check>" for every check that fails and
 * "<who>: <failed> of <checks> checks failed"; the child ends with "budget child: done".
 * One print call per line: the shell runs alongside and may print between two calls.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "rorthox-config.h"

#define PAGE 4096U
#define LIMIT ((unsigned int)PROCESS_HEAP_LIMIT_BYTES)

struct tally
{
    const char *who;
    int checks;
    int failed;
};

static unsigned int pages(unsigned int size)
{
    return (size + PAGE - 1) / PAGE * PAGE;
}

// The pages the kernel charges for the argument array and the argc argument strings.
static unsigned int arguments_charge(int argc)
{
    return pages(sizeof(char *) * argc) + argc * pages(sizeof(((struct command_argument *)0)->argument));
}

// Appends text and, if with_number, " <n>"; returns line.
static char *add(char *line, const char *text, int n, int with_number)
{
    strcpy(line + strlen(line), text);
    if (with_number)
    {
        strcpy(line + strlen(line), " ");
        strcpy(line + strlen(line), itoa(n));
    }
    return line;
}

static void check(struct tally *t, int ok, const char *text)
{
    t->checks++;
    if (!ok)
    {
        char line[160] = "";
        t->failed++;
        add(add(add(line, t->who, 0, 0), ": FAIL ", 0, 0), text, 0, 0);
        print(add(line, "\n", 0, 0));
    }
}

// A request that must be refused (one that wasn't is freed again).
static void refused(struct tally *t, unsigned int size, const char *text)
{
    void *p = malloc(size);
    if (p)
    {
        free(p);
    }
    check(t, p == 0, text);
}

static void summary(struct tally *t)
{
    char line[160] = "";
    add(add(add(line, t->who, 0, 0), ":", t->failed, 1), " of", t->checks, 1);
    print(add(line, " checks failed\n", 0, 0));
}

static int child(int argc)
{
    struct tally t = {"budget child", 0, 0};
    unsigned int rest = LIMIT - arguments_charge(argc);
    char *all = malloc(rest);
    check(&t, all != 0, "the rest of its own budget does not fit while the parent holds all of its own");
    if (all)
    {
        all[0] = 1;
        all[rest - 1] = 1;
    }
    refused(&t, 1, "one more byte admitted");
    summary(&t);
    print("budget child: done\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strncmp(argv[1], "child", 6) == 0)
    {
        return child(argc);
    }

    struct tally t = {"budget parent", 0, 0};
    // The child's command first: once the budget is full there is no room for it.
    struct command_argument *command = rorthox_parse_command("budget.elf child", 1024);
    check(&t, command != 0, "no command for the child");
    unsigned int used = arguments_charge(argc) + 2 * pages(sizeof(struct command_argument));
    unsigned int rest = LIMIT - used;
    char line[160] = "";
    print(add(add(add(line, "budget parent: budget", LIMIT, 1), ", in use", used, 1), "\n", 0, 0));

    refused(&t, rest + 1, "one byte more than the rest admitted");
    char *all = malloc(rest);
    check(&t, all != 0, "the rest of the budget does not fit");
    if (all)
    {
        all[0] = 1;
        all[rest - 1] = 1;
    }
    refused(&t, 1, "one byte over the budget admitted");

    free(all);
    all = malloc(rest - PAGE);
    check(&t, all != 0, "after a free, all but a page does not fit again");
    char *one = malloc(1);
    check(&t, one != 0, "1 byte does not fit in the last page");
    refused(&t, 1, "1 more byte admitted (1 byte must cost a whole page)");
    free(one);
    free(one);
    refused(&t, 2 * PAGE, "two pages admitted after freeing the 1-byte allocation twice");
    if (all)
    {
        free(all + PAGE);
    }
    refused(&t, 2 * PAGE, "two pages admitted after a free inside an allocation");
    one = malloc(PAGE);
    check(&t, one != 0, "freeing the 1-byte allocation did not give back a whole page");
    refused(&t, 1, "one byte admitted with the whole budget held");
    summary(&t);

    print("budget parent: holds its whole budget, starting the child\n");
    rorthox_system(command);
    print("budget parent: child started\n");
    while (1)
    {
        sleep(1000);
    }
    return 0;
}
