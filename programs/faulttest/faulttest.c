/*
 * Exception test for scripts/tests/exceptions.sh and kernel-exceptions.sh (only copied into
 * the test images, as fault.elf: the FAT16 driver has no long names).
 *
 *   fault.elf ud2|int3|cli|pf|int14|int18|in|out|in21|out20|inffff
 *                                     prints "faulttest: armed", waits for a key, then raises
 *                                     the CPU exception named by the argument (pf: allocate a
 *                                     page, free it and read it; in/out: read or write one
 *                                     byte at port 0x80, which ring 3 may not use: #GP;
 *                                     in21/out20/inffff: the same at ports 0x21, 0x20, 0xffff).
 *                                     The kernel must end the
 *                                     program, so "faulttest: survived" must never show.
 *   fault.elf parent                  starts "fault.elf child" (which then owns the keyboard)
 *                                     and raises #UD itself.
 *   fault.elf child                   prints "faulttest child: ready", waits for a key, prints
 *                                     "faulttest child: done" and exits.
 *
 * Isolation probes for scripts/tests/isolation.sh (<tag> keeps every line of a run unique):
 *   fault.elf probe <tag>             checks that its own data, stack, allocation and code work
 *                                     ("faulttest <tag>: own memory ok"), prints its mailbox, a
 *                                     page it allocated ("faulttest <tag>: box 0x..."), then
 *                                     "faulttest <tag>: armed" and waits until the test's debugger
 *                                     writes an operation and a target address into the mailbox:
 *                                     box[0] 'r' read, 'w' write, 'x' call the target, 's' hand the
 *                                     target to print/write/read/stat as a buffer (prints the
 *                                     results and exits); box[1] the target.
 *   fault.elf reuse <tag>             allocates, uses and frees a page, starts "fault.elf child"
 *                                     (the kernel's allocations for it, e.g. its process or task
 *                                     structures or page tables, take the freed page), puts
 *                                     the page's address in box[2], then works like probe.
 *   fault.elf owner <tag>             starts "fault.elf probe <tag>", then allocates a page with
 *                                     a known pattern after the probe's own pages, prints
 *                                     "faultowner <tag>: sentinel 0x..." and "... done 0x..." and
 *                                     sleeps until the test writes 1 to the done word. Then it
 *                                     prints "faultowner <tag>: sentinel intact" (or "changed").
 * An access the kernel lets through prints "faulttest <tag>: survived".
 *
 * One line per print call: the shell runs alongside and may print between two calls.
 */
#include <stdint.h>
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"

void fault_ud2(void);
void fault_int3(void);
void fault_cli(void);
uint32_t fault_read(void *ptr);
void fault_int14(void);
void fault_int18(void);
void fault_in(void);
void fault_out(void);
void fault_in_pic(void);
void fault_out_pic(void);
void fault_in_high(void);
void fault_write(void *ptr);
void fault_exec(void *ptr);
int print_result(const char *message) __asm__("print");

static int is(const char *arg, const char *name)
{
    return strncmp(arg, name, strlen(name) + 1) == 0;
}

// Bounded line building: every line goes out in one print call.
static int put(char *out, int n, const char *text)
{
    while (*text && n < 120)
    {
        out[n++] = *text++;
    }
    return n;
}

static int put_hex(char *out, int n, uint32_t value)
{
    n = put(out, n, "0x");
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        out[n++] = "0123456789abcdef"[(value >> shift) & 15];
    }
    return n;
}

static int put_int(char *out, int n, int value)
{
    char digits[12];
    int d = 0;
    unsigned int v = value < 0 ? -(unsigned int)value : (unsigned int)value;
    do
    {
        digits[d++] = '0' + v % 10;
        v /= 10;
    } while (v);
    if (value < 0)
    {
        out[n++] = '-';
    }
    while (d)
    {
        out[n++] = digits[--d];
    }
    return n;
}

// "<who> <tag>: <text>[ 0x<value>]"
static void say(const char *who, const char *tag, const char *text, int with_value, uint32_t value)
{
    char out[160];
    int n = put(out, 0, who);
    n = put(out, n, " ");
    n = put(out, n, tag);
    n = put(out, n, ": ");
    n = put(out, n, text);
    if (with_value)
    {
        n = put(out, n, " ");
        n = put_hex(out, n, value);
    }
    out[n++] = '\n';
    out[n] = 0;
    print(out);
}

static volatile uint32_t own_data = 0x5a17e001;

// Own .data, stack, allocation and code must stay usable.
static int own_memory_ok(volatile uint32_t *box)
{
    volatile uint32_t stack_word = 0x51ac0001;
    own_data = own_data + 1;
    stack_word = stack_word + 1;
    box[3] = 0xa110c001;
    volatile const uint8_t *code = (const uint8_t *)fault_ud2;
    return own_data == 0x5a17e002 && stack_word == 0x51ac0002 && box[3] == 0xa110c001 && code[0] == 0x0f && code[1] == 0x0b;
}

// Hands the target to syscalls as a buffer; each must refuse it (-2) without touching it.
static void probe_syscalls(const char *tag, void *target)
{
    int printed = print_result((const char *)target);
    int written = -100, read = -100, stat = -100;
    int fd = rorthox_open("0:/iso.txt", RORTHOX_FILE_WRITE);
    if (fd > 0)
    {
        written = rorthox_write(fd, target, 16);
        rorthox_close(fd);
    }
    fd = rorthox_open("0:/blank.elf", RORTHOX_FILE_READ);
    if (fd > 0)
    {
        read = rorthox_read(fd, target, 16);
        stat = rorthox_stat(fd, (struct rorthox_file_stat *)target);
        rorthox_close(fd);
    }
    char out[160];
    int n = put(out, 0, "faulttest ");
    n = put(out, n, tag);
    n = put(out, n, ": print ");
    n = put_int(out, n, printed);
    n = put(out, n, " write ");
    n = put_int(out, n, written);
    n = put(out, n, " read ");
    n = put_int(out, n, read);
    n = put(out, n, " stat ");
    n = put_int(out, n, stat);
    out[n++] = '\n';
    out[n] = 0;
    print(out);
}

static int probe(const char *tag, volatile uint32_t *box, uint32_t freed)
{
    if (!box)
    {
        say("faulttest", tag, "no memory", 0, 0);
        return 1;
    }
    box[0] = 0;
    box[1] = 0;
    box[2] = freed;
    say("faulttest", tag, own_memory_ok(box) ? "own memory ok" : "own memory broken", 0, 0);
    say("faulttest", tag, "box", 1, (uint32_t)box);
    say("faulttest", tag, "armed", 0, 0);
    uint32_t op;
    while (!(op = box[0]))
    {
        sleep(10);
    }
    void *target = (void *)box[1];
    if (op == 's')
    {
        probe_syscalls(tag, target);
        return 0;
    }
    if (op == 'r')
    {
        fault_read(target);
    }
    else if (op == 'w')
    {
        fault_write(target);
    }
    else if (op == 'x')
    {
        fault_exec(target);
    }
    say("faulttest", tag, "survived", 0, 0);
    return 0;
}

static void command(char *out, const char *mode, const char *tag)
{
    int n = put(out, 0, "fault.elf ");
    n = put(out, n, mode);
    n = put(out, n, " ");
    n = put(out, n, tag);
    out[n] = 0;
}

#define SENTINEL_TEXT "owner secret"

static uint8_t sentinel_byte(int i)
{
    return i < (int)sizeof(SENTINEL_TEXT) ? SENTINEL_TEXT[i] : (uint8_t)(i * 7 + 3);
}

static int owner(const char *tag)
{
    char cmd[160];
    command(cmd, "probe", tag);
    rorthox_system_run(cmd);
    // Let the probe make its own allocations first: the sentinel then lands next to them.
    sleep(500);
    uint8_t *sentinel = malloc(4096);
    volatile uint32_t *done = malloc(16);
    if (!sentinel || !done)
    {
        say("faultowner", tag, "no memory", 0, 0);
        return 1;
    }
    for (int i = 0; i < 4096; i++)
    {
        sentinel[i] = sentinel_byte(i);
    }
    done[0] = 0;
    say("faultowner", tag, "sentinel", 1, (uint32_t)sentinel);
    say("faultowner", tag, "done", 1, (uint32_t)done);
    while (!done[0])
    {
        sleep(20);
    }
    int intact = 1;
    for (int i = 0; i < 4096; i++)
    {
        intact &= sentinel[i] == sentinel_byte(i);
    }
    say("faultowner", tag, intact ? "sentinel intact" : "sentinel changed", 0, 0);
    return 0;
}

static int reuse(const char *tag)
{
    // Everything this program allocates comes first, so only the kernel can reuse the page.
    volatile uint32_t *box = malloc(16);
    char cmd[32];
    strcpy(cmd, "fault.elf child");
    struct command_argument *child = rorthox_parse_command(cmd, sizeof(cmd));
    volatile uint32_t *page = malloc(4096);
    // A live page right after it leaves a one-page hole: the kernel's first one-page allocation
    // for the child takes it, not the child's own malloc calls (they come later).
    void *guard = malloc(4096);
    if (!box || !child || !page || !guard)
    {
        say("faulttest", tag, "no memory", 0, 0);
        return 1;
    }
    page[0] = 0x7e570001;
    uint32_t freed = (uint32_t)page;
    if (page[0] != 0x7e570001)
    {
        say("faulttest", tag, "own page broken", 0, 0);
    }
    free((void *)page);
    rorthox_system(child);
    return probe(tag, box, freed);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strnlen(argv[2], 20) < 20)
    {
        if (is(argv[1], "probe"))
        {
            return probe(argv[2], malloc(16), 0);
        }
        if (is(argv[1], "reuse"))
        {
            return reuse(argv[2]);
        }
        if (is(argv[1], "owner"))
        {
            return owner(argv[2]);
        }
    }

    if (argc != 2)
    {
        print("faulttest: usage: fault.elf ud2|int3|cli|pf|int14|int18|in|out|in21|out20|inffff|parent|child\n");
        print("faulttest: usage: fault.elf probe|reuse|owner <tag>\n");
        return 1;
    }

    const char *mode = argv[1];
    if (is(mode, "child"))
    {
        print("faulttest child: ready\n");
        rorthox_getkeyblock();
        print("faulttest child: done\n");
        return 0;
    }

    if (is(mode, "parent"))
    {
        rorthox_system_run("fault.elf child");
        print("faulttest parent: faulting\n");
        fault_ud2();
        print("faulttest: survived\n");
        return 0;
    }

    // An allocation the program still holds when it faults: ending the program must free it.
    void *held = malloc(64);
    print("faulttest: armed\n");
    rorthox_getkeyblock();

    if (is(mode, "ud2"))
    {
        fault_ud2();
    }
    else if (is(mode, "int3"))
    {
        fault_int3();
    }
    else if (is(mode, "cli"))
    {
        fault_cli();
    }
    else if (is(mode, "int14"))
    {
        fault_int14();
    }
    else if (is(mode, "int18"))
    {
        fault_int18();
    }
    else if (is(mode, "in"))
    {
        fault_in();
    }
    else if (is(mode, "out"))
    {
        fault_out();
    }
    else if (is(mode, "in21"))
    {
        fault_in_pic();
    }
    else if (is(mode, "out20"))
    {
        fault_out_pic();
    }
    else if (is(mode, "inffff"))
    {
        fault_in_high();
    }
    else if (is(mode, "pf"))
    {
        // Nothing may allocate between the free and the read.
        void *page = malloc(4096);
        free(page);
        fault_read(page);
    }
    else
    {
        print("faulttest: unknown mode\n");
        free(held);
        return 1;
    }

    print("faulttest: survived\n");
    free(held);
    return 0;
}
