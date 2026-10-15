/*
 * Host check for scripts/tests/readline.sh: the real rorthox_terminal_readline()
 * (programs/stdlib/src/rorthox.c), built with the host C compiler. The script builds rorthox.c
 * and string.c on their own and puts "user_" in front of every symbol (objcopy
 * --prefix-symbols), so nothing clashes with the host C library. The key syscall
 * (user_rorthox_getkey) plays a script of keys, with a "no key yet" (0) before every key; the
 * echo syscall (user_rorthox_putchar) records what is echoed.
 *
 * Backspace at the start of the line must be ignored: not echoed (the echo would erase the
 * shell's prompt) and not stored (K26: it was stored as 0x08 and used up a place in the line).
 * Cases: backspace + Enter; backspaces before a line, with a line that exactly fills the buffer;
 * a + 2 backspaces + b; echo off; a full buffer with guard bytes around it; an ordinary erase;
 * many backspaces into a small buffer; Enter alone.
 *
 * Prints "readline host check: <failed> of <cases> cases failed".
 */
#include "host.h"

void user_rorthox_terminal_readline(char *out, int max, _Bool output_while_typing);

int strcmp(const char *a, const char *b);
void *memset(void *s, int c, unsigned long n);
unsigned long strlen(const char *s);

#define BS "\x08"
#define ENTER "\x0d"
#define GUARD 0xA5
#define BEFORE 16
#define AREA 128

static const char *keys;
static int next_key;
static int key_count;
static int zero_next;
static int overrun;
static char echo[256];
static int echo_count;

int user_rorthox_getkey()
{
    // "No key yet" before every key, so the blocking wrapper's loop runs too.
    zero_next = !zero_next;
    if (zero_next)
    {
        return 0;
    }
    if (next_key >= key_count)
    {
        overrun = 1;
        return 13;
    }
    return (unsigned char)keys[next_key++];
}

void user_rorthox_putchar(char c)
{
    if (echo_count < (int)sizeof(echo) - 1)
    {
        echo[echo_count] = c;
    }
    echo_count++;
}

// Not used by readline; rorthox.c needs them to link.
void *user_rorthox_malloc(unsigned long size)
{
    return 0;
}

int user_rorthox_system(void *arguments)
{
    return -1;
}

static int cases;
static int failed;

static void show(const char *name, const char *s, int n)
{
    printf("%s \"", name);
    for (int i = 0; i < n; i++)
    {
        unsigned char c = s[i];
        if (c >= 32 && c < 127)
        {
            printf("%c", c);
        }
        else
        {
            printf("\\x%02x", c);
        }
    }
    printf("\"");
}

// One case: <script> (<script_len> keys) into a buffer of <max>; expect <want> stored, <want_echo>
// echoed and <want_used> keys taken.
static void run(const char *what, const char *script, int script_len, int max, int output, const char *want,
                const char *want_echo, int want_used)
{
    static unsigned char area[AREA];
    char *out = (char *)area + BEFORE;

    keys = script;
    key_count = script_len;
    next_key = 0;
    zero_next = 0;
    overrun = 0;
    echo_count = 0;
    memset(echo, 0, sizeof(echo));
    memset(area, GUARD, sizeof(area));

    user_rorthox_terminal_readline(out, max, output);

    int ok = !overrun && next_key == want_used;
    // The line and its terminator, and nothing written outside out[0 .. max-1].
    int len = strlen(want);
    for (int i = 0; i < len + 1; i++)
    {
        ok = ok && out[i] == want[i];
    }
    for (int i = 0; i < AREA; i++)
    {
        if (i < BEFORE || i >= BEFORE + max)
        {
            ok = ok && area[i] == GUARD;
        }
    }
    ok = ok && echo_count == (int)strlen(want_echo) && strcmp(echo, want_echo) == 0;

    cases++;
    if (!ok)
    {
        failed++;
        printf("  FAIL %s: ", what);
        show("stored", out, max);
        printf(", ");
        show("echoed", echo, echo_count < (int)sizeof(echo) ? echo_count : (int)sizeof(echo) - 1);
        printf(", %d keys taken (expected %d)%s\n", next_key, want_used, overrun ? ", ran out of keys" : "");
    }
}

#define KEYS(s) s, (int)sizeof(s) - 1

int main(void)
{
    static char many[200];

    run("backspace, Enter", KEYS(BS ENTER), 16, 1, "", "", 2);
    run("3 backspaces, then a line", KEYS(BS BS BS "blank.elf rl1" ENTER), 64, 1, "blank.elf rl1", "blank.elf rl1",
        17);
    run("3 backspaces, then a line that fills the buffer", KEYS(BS BS BS "blank.elf" ENTER), 10, 1, "blank.elf",
        "blank.elf", 12);
    run("a, 2 backspaces, b", KEYS("a" BS BS "b" ENTER), 16, 1, "b", "a" BS "b", 5);
    run("a, 3 backspaces, b", KEYS("a" BS BS BS "b" ENTER), 16, 1, "b", "a" BS "b", 6);
    run("echo off", KEYS(BS "x" BS BS "y" ENTER), 16, 0, "y", "", 6);
    // max 4: three characters and the terminator; the 'd' stays unread.
    run("full buffer", KEYS(BS BS "abcd" ENTER), 4, 1, "abc", "abc", 5);
    run("ordinary erase", KEYS("ab" BS "c" ENTER), 16, 1, "ac", "ab" BS "c", 5);
    run("Enter alone", KEYS(ENTER), 16, 1, "", "", 1);

    memset(many, 0x08, 150);
    many[150] = 'x';
    many[151] = 'y';
    many[152] = 0x0d;
    run("150 backspaces into a buffer of 3", many, 153, 3, 1, "xy", "xy", 152);

    printf("readline host check: %d of %d cases failed\n", failed, cases);
    fflush(0);
    return failed ? 1 : 0;
}
