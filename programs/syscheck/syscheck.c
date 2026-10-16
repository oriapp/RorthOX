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

static struct process_arguments writable = {123, (void *)456};
static struct process_arguments zeroed;
static const unsigned char readonly[16] = "do not overwrite";
int bad_stack(int command, unsigned int sp);

static int same(const volatile unsigned char *a, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int arguments_ok(struct process_arguments *p, int argc, char **argv)
{
    return get_arguments(p) == 0 && p->argc == argc && p->argv == argv;
}
static int arguments_test(int argc, char **argv)
{
    struct process_arguments stack;
    unsigned char *heap = malloc(8192);
    if (!heap) return 1;
    int bad = !arguments_ok(&stack, argc, argv);
    bad |= !arguments_ok((void *)(heap + 3), argc, argv);
    bad |= !arguments_ok((void *)(heap + 4093), argc, argv);
    bad |= !arguments_ok(&writable, argc, argv);
    bad |= !arguments_ok(&zeroed, argc, argv);
    unsigned char before[16];
    for (int i = 0; i < 16; i++) before[i] = ((volatile unsigned char *)readonly)[i];
    int ro = get_arguments((void *)readonly);
    int unchanged = same(readonly, before, 16);
    printf("syscheck: readonly %i unchanged %i\n", ro, unchanged);
    bad |= ro != -2 || !unchanged;
    volatile unsigned char *outside = (void *)0x800000;
    for (int i = 0; i < 16; i++) before[i] = outside[i];
    int unowned = get_arguments((void *)outside);
    unchanged = same(outside, before, 16);
    printf("syscheck: outside %i unchanged %i\n", unowned, unchanged);
    bad |= unowned != -2 || !unchanged;
    /* Only four of eight bytes are owned: refusal must not store argc first. */
    unsigned char *tiny = malloc(4);
    if (!tiny) return 1;
    memset(tiny, 0x5a, 4);
    bad |= get_arguments((void *)tiny) != -2;
    for (int i = 0; i < 4; i++) bad |= tiny[i] != 0x5a;
    bad |= !arguments_ok(&stack, argc, argv);
    free(tiny); free(heap);
    return bad;
}

static int freed_arguments_test(const char *mode, int argc, char **argv)
{
    unsigned char *memory = malloc(8192);
    struct process_arguments stack;
    if (!memory) return 1;
    /* Keep the operand in the second page, away from one-block print buffers. */
    unsigned int address = (unsigned int)(memory + 4096);
    *(unsigned int *)(memory + 4096) = (unsigned int)&stack;
    free(memory);
    int rc = !strncmp(mode, "freesp", 7) ?
        bad_stack(SYSCALL_GET_PROGRAM_ARGUMENTS, address) : get_arguments((void *)address);
    printf("syscheck: freed result %i\n", rc);
    return rc != -2 || !arguments_ok(&stack, argc, argv);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    if (!strncmp(argv[1], "free", 4))
    {
        armed();
        print(freed_arguments_test(argv[1], argc, argv) ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: continued\n");
        return 0;
    }
    if (!strncmp(argv[1], "get", 4))
    {
        armed();
        print(arguments_test(argc, argv) ? "syscheck: FAIL\n" : "syscheck: PASS\n");
        print("syscheck: continued\n");
        return 0;
    }
    if (!strncmp(argv[1], "badsp", 6))
    {
        armed();
        int rc = bad_stack(SYSCALL_GET_PROGRAM_ARGUMENTS, 0xfffffffeU);
        if (get_arguments((void *)0xfffffffcU) != -2) rc = 1;
        printf("syscheck: result %i\n", rc);
        print("syscheck: continued\n");
        return 0;
    }
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
