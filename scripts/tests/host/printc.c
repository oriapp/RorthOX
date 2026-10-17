/*
 * Host check for scripts/tests/printc.sh: the real printc syscall handler
 * (isr80h_command11_printc in kernel/isr80h/io.c), built with the host C compiler. The task's
 * stack items, copy_string_from_task() and the terminal are stubs that record what the handler
 * does.
 *
 * The user wrapper passes the message as stack item 0 and the colour as stack item 1, an
 * integer (K19: the wrapper used to pass the message twice and the handler copied item 1 as a
 * string and ran atoi() on it).
 *  - colours 0-15: exactly one copy, from item 0, never from the colour; the copied text is
 *    printed once in that colour; result 0.
 *  - colours 16, 255, 256, 0x7fffffff, 0x80000000, 0xffffffff: result -EINVARG, nothing copied,
 *    nothing printed.
 *  - a failing copy (-EIO, -EINVARG, -ENOMEM) with a valid colour: the same error comes back and
 *    nothing is printed.
 *
 * Two more handlers in the same file:
 *  - getkey (syscall 2): every key value (bytes 0x01-0xff, the RORTHOX_KEY_* values, 0xffff, and
 *    0 for "no key") comes back whole and zero-extended, never through a char.
 *  - the cursor move (syscall 11): stack item 0 goes to terminal_move_cursor() as a signed int,
 *    exactly once, and its result comes back; when reading item 0 fails, that error comes back
 *    and the terminal is not called.
 *
 * Prints "printc host check: <failed> of <cases> cases failed".
 */
#include <stdint.h>

#include "isr80h/io.c"

#include "host.h"

int strcmp(const char *a, const char *b);
char *strcpy(char *dest, const char *src);

#define MESSAGE_ADDRESS 0x401234UL
#define MESSAGE "colour text"

static struct task *the_task = (struct task *)0x1000;
static uint32_t stack_items[2];
static int stack_error;

static int copies;
static unsigned long copy_source;
static int copy_result;
static int printc_calls;
static unsigned int printc_colour;
static char printc_text[64];
static int other_output;

struct task *task_current()
{
    return the_task;
}

int task_get_stack_item(struct task *task, int index, uint32_t *value)
{
    if (task != the_task || index < 0 || index > 1)
    {
        other_output++;
        return -EINVARG;
    }
    if (stack_error < 0)
    {
        return stack_error;
    }
    *value = stack_items[index];
    return 0;
}

int copy_string_from_task(struct task *task, void *virtual, void *phys, int max)
{
    copies++;
    copy_source = (unsigned long)(uintptr_t)virtual;
    if (copy_result < 0)
    {
        return copy_result;
    }
    if (max < (int)sizeof(MESSAGE))
    {
        // A copy into a buffer too small for the message (the old two-byte colour buffer).
        ((char *)phys)[0] = 0;
        return 0;
    }
    strcpy(phys, MESSAGE);
    return 0;
}

void printc(const char *str, unsigned int color)
{
    printc_calls++;
    printc_colour = color;
    printc_text[0] = 0;
    if (strlen(str) < sizeof(printc_text))
    {
        strcpy(printc_text, str);
    }
}

void print(const char *str)
{
    other_output++;
}

void terminal_writechar(char c, char colour)
{
    other_output++;
}

static uint16_t next_key;
static int pops;
uint16_t keyboard_pop()
{
    pops++;
    return next_key;
}

static int moves;
static int move_delta;
static int move_result;
int terminal_move_cursor(int delta)
{
    other_output++;
    moves++;
    move_delta = delta;
    return move_result;
}

static int cases;
static int failed;

static int run(uint32_t colour, int copy_res)
{
    stack_items[0] = MESSAGE_ADDRESS;
    stack_items[1] = colour;
    stack_error = 0;
    copies = 0;
    copy_source = 0;
    copy_result = copy_res;
    printc_calls = 0;
    printc_colour = 0xdead;
    printc_text[0] = 0;
    other_output = 0;
    return (int)(intptr_t)isr80h_command11_printc(0);
}

static void result(int ok, const char *what, uint32_t colour, int res)
{
    cases++;
    if (!ok)
    {
        failed++;
        printf("  FAIL %s: colour 0x%x: result %d, %d copies (last from 0x%lx), %d printc calls (colour %u, \"%s\"), "
               "%d other calls\n",
               what, colour, res, copies, copy_source, printc_calls, printc_colour, printc_text, other_output);
    }
}

int main(void)
{
    static const uint32_t bad[] = {16, 255, 256, 0x7fffffff, 0x80000000, 0xffffffff};
    static const int copy_errors[] = {-EIO, -EINVARG, -ENOMEM};
    uint32_t colour;
    int i;
    int res;

    for (colour = 0; colour <= 15; colour++)
    {
        res = run(colour, 0);
        result(res == 0 && copies == 1 && copy_source == MESSAGE_ADDRESS && printc_calls == 1 &&
                   printc_colour == colour && strcmp(printc_text, MESSAGE) == 0 && other_output == 0,
               "valid colour", colour, res);
    }
    printf("valid colours 0-15: one copy from the message, printed in that colour\n");

    for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++)
    {
        res = run(bad[i], 0);
        result(res == -EINVARG && copies == 0 && printc_calls == 0 && other_output == 0, "invalid colour", bad[i],
               res);
    }
    printf("invalid colours: -EINVARG, nothing copied or printed\n");

    for (i = 0; i < (int)(sizeof(copy_errors) / sizeof(copy_errors[0])); i++)
    {
        res = run(12, copy_errors[i]);
        result(res == copy_errors[i] && copies == 1 && copy_source == MESSAGE_ADDRESS && printc_calls == 0 &&
                   other_output == 0,
               "failing copy", 12, res);
    }
    printf("failing copy: its error comes back, nothing printed\n");

    static const uint16_t key_values[] = {0, 1, 0x61, 0x7f, 0x80, 0xff, RORTHOX_KEY_UP, RORTHOX_KEY_DOWN,
                                          RORTHOX_KEY_LEFT, RORTHOX_KEY_RIGHT, RORTHOX_KEY_HOME, RORTHOX_KEY_END,
                                          RORTHOX_KEY_INSERT, RORTHOX_KEY_DELETE, RORTHOX_KEY_PAGE_UP,
                                          RORTHOX_KEY_PAGE_DOWN, 0xffff};
    for (i = 0; i < (int)(sizeof(key_values) / sizeof(key_values[0])); i++)
    {
        next_key = key_values[i];
        pops = 0;
        other_output = 0;
        uintptr_t eax = (uintptr_t)isr80h_command2_getkey(0);
        result(eax == key_values[i] && pops == 1 && other_output == 0, "getkey value", key_values[i], (int)eax);
    }
    printf("getkey: every key value comes back whole\n");

    static const struct
    {
        uint32_t item;
        int tty_result, stack_error;
    } move_cases[] = {
        {0xfffffffd, 0, 0},   {3, 0, 0},           {0, 0, 0},           {0x80000000, -EINVARG, 0},
        {0x7fffffff, -EINVARG, 0}, {80, -EINVARG, 0}, {5, 0, -EINVARG}, {5, 0, -EIO},
    };
    for (i = 0; i < (int)(sizeof(move_cases) / sizeof(move_cases[0])); i++)
    {
        stack_items[0] = move_cases[i].item;
        stack_error = move_cases[i].stack_error;
        move_result = move_cases[i].tty_result;
        moves = 0;
        move_delta = 0x5a5a5a5a;
        other_output = 0;
        res = (int)(intptr_t)isr80h_command_terminal_move_cursor(0);
        if (move_cases[i].stack_error)
            result(res == move_cases[i].stack_error && moves == 0 && other_output == 0, "cursor move, item 0 unreadable",
                   move_cases[i].item, res);
        else
            result(res == move_cases[i].tty_result && moves == 1 && move_delta == (int)move_cases[i].item &&
                       other_output == 1,
                   "cursor move", move_cases[i].item, res);
    }
    stack_error = 0;
    printf("cursor move: item 0 passed once as a signed int, an unreadable item calls nothing\n");

    printf("printc host check: %d of %d cases failed\n", failed, cases);
    fflush(0);
    return failed ? 1 : 0;
}
