/*
 * User library test program for scripts/tests/printc.sh, scripts/tests/strtok.sh and the other
 * tests named below. Only copied into those tests' images.
 *
 * libtest.elf printc
 *     Prints "libtest printc: armed" and waits for a key. Then prints a newline (the shell's
 *     prompt can land after the armed line) and "lt-c:", calls printc with
 *     "#0" ... "#f" in colours 0 to 15 (each text names its colour in hex), calls printc with
 *     "XX" and the invalid colours 16, 255, 256, 0x80000000 and 0xffffffff, and prints "|end"
 *     and a newline. On the screen that is one row: "lt-c:#0#1...#f|end", every "#n" in colour
 *     n on black, the rest white on black, and no "X". Every printc call runs with marker
 *     values in ebx, esi, edi and ebp, which the wrapper must keep (cdecl). Then one line per
 *     printc result that is wrong ("libtest printc: FAIL ..."), "libtest printc: FAIL registers
 *     changed ..." or "libtest printc: ebx esi edi ebp kept", "libtest printc: results checked",
 *     and "libtest printc: done"; waits for a key and ends.
 *
 * libtest.elf strtok
 *     Runs strtok on a few strings, starting a new string while the last one isn't finished
 *     (that must start over), and checks where every token starts and what it reads. Prints
 *     "libtest strtok: start", a FAIL line for every wrong step and
 *     "libtest strtok: <failed> of <checks> checks failed", and ends.
 *
 * libtest.elf keys
 *     Prints "libtest keys: armed" and reads raw key values with getkeyblock. Every 'z' ends a
 *     group: it prints "lt-k<n>:" and the values read since the last group in hex
 *     ("lt-k1: 100 101"). A 'q' ends the last group, then "libtest keys: done".
 *
 * libtest.elf cursor
 *     Prints "libtest cursor: armed" and waits for a key. Then prints "\nlt-m:ABCDE", moves
 *     the cursor -3, prints "x", moves +1, prints "y", tries the moves INT_MIN, INT_MAX, +2000
 *     and -2000 (all off the page), prints "|", a newline, moves -1 (column 79 of the row
 *     above) and prints "RS". The rows read "lt-m:ABxDy|" with R in column 79, and "S".
 *     Then "lt-m: results" and the eight results (0 0 0 0 -2 -2 -2 -2 expected in that order:
 *     the four valid moves, then the four refused ones), a FAIL line if one is wrong, the
 *     ebx/esi/edi/ebp line as for printc, and "libtest cursor: done".
 *
 * libtest.elf readline
 *     Prints "libtest readline: armed" and waits for a key. Then prints 24 newlines (the
 *     screen scrolls, so the next row is the bottom one) and a 72-character prompt
 *     "lt-r....> ", and reads an echoed line of at most 99 characters, which wraps onto the next
 *     row and scrolls the screen. Then prints "|" (where the cursor is after the read), and
 *     "\nlt-r: [<line>]" and "libtest readline: done".
 *
 * libtest.elf sleep <ticks> <current_task> <state offset>
 *     Sleep (syscall 12), for scripts/tests/sleep.sh. The arguments are hex: the addresses of
 *     the kernel's tick counter and current_task, and the offset of the state field in struct
 *     task (ring 3 can read kernel memory). Prints "libtest sleep: armed" and waits for a key.
 *     Then "lt-s: zero 0 negative -2 high -2" (sleep(0), sleep(-1), sleep(0x80000000)), two
 *     sleep(50) with their results and ticks, "lt-s: short sleeps ok" if both returned 0 after at
 *     least 7 ticks, starts "libtest.elf sleeppeer" and runs sleep(1000): "lt-s: slept r <r>
 *     ticks <n> from <t0> to <t1>" and "lt-s: long sleep ok" for r 0 and n >= 102. Every sleep
 *     runs with the register and stack markers of printc; "lt-s: ebx esi edi ebp esp and stack
 *     kept". Ends with "libtest sleep: done" (or FAIL lines and "libtest sleep: FAIL").
 *
 * libtest.elf sleeppeer <ticks> <task> <state offset>
 *     Started by sleep. Waits until that task's state is 1 (sleeping), then counts the ticks it
 *     runs on until the state changes: "lt-p: ran on <n> ticks while the sleeper slept, from <first>
 *     to <last>".
 *
 * Started by the kernel as shell.elf (no arguments at all), for scripts/tests/sleep-idle.sh
 *     Prints "libtest idle: armed" and reads a line with the hex addresses of the tick counter
 *     and current_task. Prints "lt-i: task <its task>" and "lt-i: sleeping", sleeps 5000 ms as
 *     the only task, reads one key and prints "lt-i: woke r <r> ticks <n> key <key>" and
 *     "lt-i: first sleep ok" (r 0, n >= 502, key 'k'). Then starts "libtest.elf idlechild",
 *     prints "lt-i: second sleeping", sleeps 3000 ms, "lt-i: second sleep ok" (n >= 302) and
 *     "libtest idle: done", and sleeps on forever (the last process must not exit).
 *
 * libtest.elf idlechild <ticks> <current_task>
 *     Prints "lt-ic: task <its task> sleeping", sleeps 6000 ms, prints its result and ticks and
 *     "lt-ic: sleep ok" (r 0, n >= 602), and ends.
 */
#include "rorthox.h"
#include "stdlib.h"
#include "string.h"
#include "status.h"

// Registers that a checked call changed so far: 1 ebx, 2 esi, 4 edi, 8 ebp, 16 esp not back
// where it was, 32 the stack word above the arguments overwritten.
static unsigned int registers_changed;
// esp right before a checked call pushes its arguments.
unsigned int lt_call_esp;

// fn(a, b), called with marker values in ebx, esi, edi and ebp, which a cdecl function must
// give back unchanged, and a marker word on the stack above its arguments. Returns the int the
// syscall leaves in eax.
static int call_checked(void *fn, const void *a, unsigned int b)
{
    int result = (int)fn;
    unsigned int changed = b;
    const void *str = a;
    __asm__ volatile("push %%ebp\n\t"
                     "push %%ebx\n\t"
                     "push %%esi\n\t"
                     "push %%edi\n\t"
                     "push $0x5c0ffee5\n\t"
                     "mov %%esp, lt_call_esp\n\t"
                     "push %%ecx\n\t"
                     "push %%edx\n\t"
                     "mov $0x5eb15eb1, %%ebx\n\t"
                     "mov $0x5e515e51, %%esi\n\t"
                     "mov $0x5ed15ed1, %%edi\n\t"
                     "mov $0x5eb05eb0, %%ebp\n\t"
                     "call *%%eax\n\t"
                     "add $8, %%esp\n\t"
                     "xor %%ecx, %%ecx\n\t"
                     "cmp $0x5eb15eb1, %%ebx\n\t"
                     "je 1f\n\t"
                     "or $1, %%ecx\n"
                     "1:\n\t"
                     "cmp $0x5e515e51, %%esi\n\t"
                     "je 2f\n\t"
                     "or $2, %%ecx\n"
                     "2:\n\t"
                     "cmp $0x5ed15ed1, %%edi\n\t"
                     "je 3f\n\t"
                     "or $4, %%ecx\n"
                     "3:\n\t"
                     "cmp $0x5eb05eb0, %%ebp\n\t"
                     "je 4f\n\t"
                     "or $8, %%ecx\n"
                     "4:\n\t"
                     "cmp lt_call_esp, %%esp\n\t"
                     "je 5f\n\t"
                     "or $16, %%ecx\n"
                     "5:\n\t"
                     "cmpl $0x5c0ffee5, (%%esp)\n\t"
                     "je 6f\n\t"
                     "or $32, %%ecx\n"
                     "6:\n\t"
                     "add $4, %%esp\n\t"
                     "pop %%edi\n\t"
                     "pop %%esi\n\t"
                     "pop %%ebx\n\t"
                     "pop %%ebp"
                     : "+a"(result), "+c"(changed), "+d"(str)
                     :
                     : "memory", "cc");
    registers_changed |= changed;
    return result;
}

static int printc_checked(const char *str, unsigned int color)
{
    return call_checked(printc, str, color);
}

// One argument: cdecl ignores the second one pushed.
static int move_checked(int delta)
{
    return call_checked(rorthox_terminal_move_cursor, (const void *)delta, 0);
}

static void fail(const char *mode, const char *what, unsigned int value, int got, int expected)
{
    char line[120];
    strcpy(line, "libtest ");
    strcpy(line + strlen(line), mode);
    strcpy(line + strlen(line), ": FAIL ");
    strcpy(line + strlen(line), what);
    strcpy(line + strlen(line), " ");
    strcpy(line + strlen(line), itoa((int)value));
    strcpy(line + strlen(line), " got ");
    strcpy(line + strlen(line), itoa(got));
    strcpy(line + strlen(line), " expected ");
    strcpy(line + strlen(line), itoa(expected));
    strcpy(line + strlen(line), "\n");
    print(line);
}

static int colours(void)
{
    static const unsigned int bad[] = {16, 255, 256, 0x80000000u, 0xffffffffu};
    int valid[16];
    int invalid[5];
    int failed = 0;

    print("libtest printc: armed\n");
    rorthox_getkeyblock();

    // Start the colour row on a line of its own, even if the shell's "> " came after "armed".
    print("\nlt-c:");
    for (int c = 0; c < 16; c++)
    {
        char text[3];
        text[0] = '#';
        text[1] = c < 10 ? '0' + c : 'a' + c - 10;
        text[2] = 0;
        valid[c] = printc_checked(text, c);
    }
    for (int i = 0; i < 5; i++)
    {
        invalid[i] = printc_checked("XX", bad[i]);
    }
    print("|end\n");

    for (int c = 0; c < 16; c++)
    {
        if (valid[c] != 0)
        {
            fail("printc", "colour", c, valid[c], 0);
            failed = 1;
        }
    }
    for (int i = 0; i < 5; i++)
    {
        if (invalid[i] != -EINVARG)
        {
            fail("printc", "colour", bad[i], invalid[i], -EINVARG);
            failed = 1;
        }
    }
    if (registers_changed)
    {
        fail("printc", "registers changed, mask", registers_changed, registers_changed, 0);
        failed = 1;
    }
    else
    {
        print("libtest printc: ebx esi edi ebp kept\n");
    }
    print("libtest printc: results checked\n");
    print("libtest printc: done\n");
    rorthox_getkeyblock();
    return failed;
}

static int token_checks;
static int token_failed;

// One strtok call: the token must start at want (0: no token) and read want_text.
static void token(int step, char *got, char *want, const char *want_text)
{
    token_checks++;
    if (got == want && (!want || strncmp(got, want_text, 32) == 0))
    {
        return;
    }
    token_failed++;
    // got: the token's distance from want, -1 for no token, 1000 for a token where none was expected
    fail("strtok", "step", step, !got ? -1 : want ? (int)(got - want) : 1000, 0);
}

static int tokens(void)
{
    char old[] = "old tail";
    char words[] = "new words";
    char single[] = "single";
    char next[] = "next one";
    char empty[] = "";
    char after_empty[] = "x y";
    char unfinished[] = "aa bb";
    char spaced[] = "  lead  and   repeat ";
    char mixed[] = ",a,;b;;c;";
    char again[] = "again";

    print("libtest strtok: start\n");
    // A new string while the last one isn't finished starts over.
    token(1, strtok(old, " "), old, "old");
    token(2, strtok(words, " "), words, "new");
    token(3, strtok(0, " "), words + 4, "words");
    token(4, strtok(0, " "), 0, 0);
    // The same after a string with one token.
    token(5, strtok(single, " "), single, "single");
    token(6, strtok(next, " "), next, "next");
    token(7, strtok(0, " "), next + 5, "one");
    token(8, strtok(0, " "), 0, 0);
    // Empty input, then a new string.
    token(9, strtok(empty, " "), 0, 0);
    token(10, strtok(0, " "), 0, 0);
    token(11, strtok(after_empty, " "), after_empty, "x");
    token(12, strtok(0, " "), after_empty + 2, "y");
    token(13, strtok(0, " "), 0, 0);
    // An empty string while another one isn't finished.
    token(14, strtok(unfinished, " "), unfinished, "aa");
    token(15, strtok(empty, " "), 0, 0);
    token(16, strtok(0, " "), 0, 0);
    // Leading, repeated and trailing delimiters, two delimiter characters.
    token(17, strtok(spaced, " "), spaced + 2, "lead");
    token(18, strtok(0, " "), spaced + 8, "and");
    token(19, strtok(0, " "), spaced + 14, "repeat");
    token(20, strtok(0, " "), 0, 0);
    token(21, strtok(mixed, ",;"), mixed + 1, "a");
    token(22, strtok(0, ",;"), mixed + 4, "b");
    token(23, strtok(0, ",;"), mixed + 7, "c");
    token(24, strtok(0, ",;"), 0, 0);
    // After the end, more calls give nothing until a new string comes.
    token(25, strtok(0, ",;"), 0, 0);
    token(26, strtok(again, " "), again, "again");
    token(27, strtok(0, " "), 0, 0);

    char line[80];
    strcpy(line, "libtest strtok: ");
    strcpy(line + strlen(line), itoa(token_failed));
    strcpy(line + strlen(line), " of ");
    strcpy(line + strlen(line), itoa(token_checks));
    strcpy(line + strlen(line), " checks failed\n");
    print(line);
    return token_failed != 0;
}

// Appends value in hex (no leading zeros) to line.
static void append_hex(char *line, unsigned int value)
{
    char digits[9];
    int n = 0;
    do
    {
        digits[n++] = "0123456789abcdef"[value % 16];
        value /= 16;
    } while (value);
    int k = strlen(line);
    while (n)
    {
        line[k++] = digits[--n];
    }
    line[k] = 0;
}

static int raw_keys(void)
{
    char line[400];
    int group = 1;

    print("libtest keys: armed\n");
    while (1)
    {
        strcpy(line, "lt-k");
        strcpy(line + strlen(line), itoa(group));
        strcpy(line + strlen(line), ":");
        int key;
        int count = 0;
        // Keep the whole value in an int: special keys are above 255.
        while ((key = rorthox_getkeyblock()) != 'z' && key != 'q')
        {
            if (count++ < 60)
            {
                strcpy(line + strlen(line), " ");
                append_hex(line, (unsigned int)key);
            }
        }
        strcpy(line + strlen(line), "\n");
        print(line);
        if (key == 'q')
        {
            break;
        }
        group++;
    }
    print("libtest keys: done\n");
    return 0;
}

static int cursor(void)
{
    static const int expected[8] = {0, 0, 0, 0, -EINVARG, -EINVARG, -EINVARG, -EINVARG};
    int result[8];
    int failed = 0;

    print("libtest cursor: armed\n");
    rorthox_getkeyblock();

    print("\nlt-m:ABCDE");
    result[0] = move_checked(-3);
    print("x");
    result[1] = move_checked(1);
    print("y");
    result[4] = move_checked(-2147483647 - 1);
    result[5] = move_checked(2147483647);
    result[6] = move_checked(2000);
    result[7] = move_checked(-2000);
    print("|");
    print("\n");
    result[2] = move_checked(-1);
    print("RS");
    result[3] = move_checked(0);
    print("\n");

    char line[120];
    strcpy(line, "lt-m: results");
    for (int i = 0; i < 8; i++)
    {
        strcpy(line + strlen(line), " ");
        strcpy(line + strlen(line), itoa(result[i]));
    }
    strcpy(line + strlen(line), "\n");
    print(line);
    for (int i = 0; i < 8; i++)
    {
        if (result[i] != expected[i])
        {
            fail("cursor", "move", i, result[i], expected[i]);
            failed = 1;
        }
    }
    if (registers_changed)
    {
        fail("cursor", "registers changed, mask", registers_changed, registers_changed, 0);
        failed = 1;
    }
    else
    {
        print("libtest cursor: ebx esi edi ebp kept\n");
    }
    print("libtest cursor: done\n");
    return failed;
}

static int edit_line(void)
{
    char prompt[80];
    char buf[100];
    char line[140];

    print("libtest readline: armed\n");
    rorthox_getkeyblock();

    for (int i = 0; i < 24; i++)
    {
        print("\n");
    }
    strcpy(prompt, "lt-r");
    int k = strlen(prompt);
    while (k < 70)
    {
        prompt[k++] = '.';
    }
    prompt[k++] = '>';
    prompt[k++] = ' ';
    prompt[k] = 0;
    print(prompt);
    rorthox_terminal_readline(buf, sizeof(buf), true);
    print("|");
    strcpy(line, "\nlt-r: [");
    strcpy(line + strlen(line), buf);
    strcpy(line + strlen(line), "]\n");
    print(line);
    print("libtest readline: done\n");
    return 0;
}

// ---- sleep (syscall 12) ----

// The kernel's tick counter. Ring 3 can read kernel memory (read-only identity map), so the
// tests pass its address and the program times its own sleeps in ticks. Once user tasks lose
// read access to kernel memory, these reads page fault and sleep.sh / sleep-idle.sh fail (never
// pass by accident); the sleep modes then need another clock source.
static volatile unsigned int *ticks;

static unsigned int hex_value(const char *s)
{
    unsigned int v = 0;
    for (; *s; s++)
    {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
        if (d < 0)
        {
            break;
        }
        v = v * 16 + d;
    }
    return v;
}

static void add(char *line, const char *s)
{
    strcpy(line + strlen(line), s);
}

static void add_int(char *line, int v)
{
    add(line, " ");
    add(line, itoa(v));
}

static void add_hex(char *line, unsigned int v)
{
    char text[10];
    int n = 0;
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        text[n++] = "0123456789abcdef"[(v >> shift) & 0xf];
    }
    text[n] = 0;
    add(line, " ");
    add(line, text);
}

static int sleep_checked(int milliseconds)
{
    return call_checked(sleep, (const void *)milliseconds, 0);
}

// One sleep, timed in ticks: its result in *result, the ticks it took as the return value.
static unsigned int timed_sleep(int milliseconds, int *result)
{
    unsigned int start = *ticks;
    *result = sleep_checked(milliseconds);
    return *ticks - start;
}

// Ticks a sleep must take at least: ceil(ms / 10) + 2 at 100 Hz.
static unsigned int least_ticks(int milliseconds)
{
    return (milliseconds + 9) / 10 + 2;
}

// libtest.elf sleep <ticks> <current_task> <state offset>: see the top of the file.
static int sleeper(int argc, char **argv)
{
    char line[160];
    int failed = 0;
    if (argc != 5)
    {
        print("libtest sleep: usage libtest.elf sleep <ticks> <current_task> <state offset>\n");
        return 1;
    }
    ticks = (volatile unsigned int *)hex_value(argv[2]);
    print("libtest sleep: armed\n");
    rorthox_getkeyblock();

    int zero = sleep_checked(0);
    int negative = sleep_checked(-1);
    int high = sleep_checked((int)0x80000000u);
    strcpy(line, "lt-s: zero");
    add_int(line, zero);
    add(line, " negative");
    add_int(line, negative);
    add(line, " high");
    add_int(line, high);
    add(line, "\n");
    print(line);
    failed |= zero != 0 || negative != -EINVARG || high != -EINVARG;

    int r1, r2;
    unsigned int t1 = timed_sleep(50, &r1);
    unsigned int t2 = timed_sleep(50, &r2);
    strcpy(line, "lt-s: short sleeps r");
    add_int(line, r1);
    add_int(line, r2);
    add(line, " ticks");
    add_int(line, t1);
    add_int(line, t2);
    add(line, "\n");
    print(line);
    int ok = r1 == 0 && r2 == 0 && t1 >= least_ticks(50) && t2 >= least_ticks(50);
    print(ok ? "lt-s: short sleeps ok\n" : "lt-s: FAIL short sleeps\n");
    failed |= !ok;

    // The peer watches this task's state while it sleeps and times its own run.
    unsigned int me = *(volatile unsigned int *)hex_value(argv[3]);
    strcpy(line, "libtest.elf sleeppeer ");
    add(line, argv[2]);
    add_hex(line, me);
    add(line, " ");
    add(line, argv[4]);
    rorthox_system_run(line);

    int r;
    unsigned int start = *ticks;
    r = sleep_checked(1000);
    unsigned int end = *ticks;
    strcpy(line, "lt-s: slept r");
    add_int(line, r);
    add(line, " ticks");
    add_int(line, end - start);
    add(line, " from");
    add_int(line, start);
    add(line, " to");
    add_int(line, end);
    add(line, "\n");
    print(line);
    ok = r == 0 && end - start >= least_ticks(1000);
    print(ok ? "lt-s: long sleep ok\n" : "lt-s: FAIL long sleep\n");
    failed |= !ok;

    if (registers_changed)
    {
        fail("sleep", "registers changed, mask", registers_changed, registers_changed, 0);
        failed = 1;
    }
    else
    {
        print("lt-s: ebx esi edi ebp esp and stack kept\n");
    }
    print(failed ? "libtest sleep: FAIL\n" : "libtest sleep: done\n");
    return failed;
}

// libtest.elf sleeppeer <ticks> <task> <state offset>: started by the sleeper. Waits for that
// task to sleep, then counts the ticks it runs on until the task is runnable again.
// The tick read sits between two state reads that both say sleeping, so the task slept at that tick.
static int asleep_at(volatile unsigned int *state, unsigned int *now)
{
    if (*state != 1)
    {
        return 0;
    }
    *now = *ticks;
    return *state == 1;
}

static int sleep_peer(int argc, char **argv)
{
    char line[160];
    if (argc != 5)
    {
        return 1;
    }
    ticks = (volatile unsigned int *)hex_value(argv[2]);
    volatile unsigned int *state = (volatile unsigned int *)(hex_value(argv[3]) + hex_value(argv[4]));
    unsigned int start = *ticks;
    unsigned int now;
    while (!asleep_at(state, &now))
    {
        if (*ticks - start > 1000)
        {
            print("lt-p: the sleeper never slept\n");
            return 1;
        }
    }
    unsigned int first = now;
    unsigned int last = now;
    int count = 1;
    while (last - first < 3000 && asleep_at(state, &now))
    {
        if (now != last)
        {
            count++;
            last = now;
        }
    }
    strcpy(line, "lt-p: ran on");
    add_int(line, count);
    add(line, " ticks while the sleeper slept, from");
    add_int(line, first);
    add(line, " to");
    add_int(line, last);
    add(line, "\n");
    print(line);
    return 0;
}

// Started by the kernel as shell.elf (no arguments): see the top of the file.
static int idle_boot(void)
{
    char line[160];
    char input[64];
    int failed = 0;
    print("libtest idle: armed\n");
    rorthox_terminal_readline(input, sizeof(input), true);
    char *ticks_text = strtok(input, " ");
    char *current_text = strtok(NULL, " ");
    if (!ticks_text || !current_text)
    {
        print("\nlibtest idle: FAIL no addresses\n");
        while (1)
        {
            sleep(100000);
        }
    }
    ticks = (volatile unsigned int *)hex_value(ticks_text);
    strcpy(line, "\nlt-i: task");
    add_hex(line, *(volatile unsigned int *)hex_value(current_text));
    add(line, "\n");
    print(line);

    // Alone: the kernel halts until the deadline. A key typed meanwhile waits in the queue.
    print("lt-i: sleeping\n");
    int r;
    unsigned int t = timed_sleep(5000, &r);
    int key = rorthox_getkey();
    strcpy(line, "lt-i: woke r");
    add_int(line, r);
    add(line, " ticks");
    add_int(line, t);
    add(line, " key");
    add_int(line, key);
    add(line, "\n");
    print(line);
    int ok = r == 0 && t >= least_ticks(5000) && key == 'k';
    print(ok ? "lt-i: first sleep ok\n" : "lt-i: FAIL first sleep\n");
    failed |= !ok;

    // With a child that sleeps longer: both asleep, this one wakes first.
    strcpy(line, "libtest.elf idlechild ");
    add(line, ticks_text);
    add(line, " ");
    add(line, current_text);
    rorthox_system_run(line);
    print("lt-i: second sleeping\n");
    t = timed_sleep(3000, &r);
    strcpy(line, "lt-i: second woke r");
    add_int(line, r);
    add(line, " ticks");
    add_int(line, t);
    add(line, "\n");
    print(line);
    ok = r == 0 && t >= least_ticks(3000);
    print(ok ? "lt-i: second sleep ok\n" : "lt-i: FAIL second sleep\n");
    failed |= !ok;
    if (registers_changed)
    {
        fail("idle", "registers changed, mask", registers_changed, registers_changed, 0);
        failed = 1;
    }
    print(failed ? "libtest idle: FAIL\n" : "libtest idle: done\n");

    // The last process must not exit (that is a kernel panic): sleep on.
    while (1)
    {
        sleep(100000);
    }
    return 0;
}

// libtest.elf idlechild <ticks> <current_task>: started by idle_boot.
static int idle_child(int argc, char **argv)
{
    char line[160];
    if (argc != 4)
    {
        return 1;
    }
    ticks = (volatile unsigned int *)hex_value(argv[2]);
    strcpy(line, "lt-ic: task");
    add_hex(line, *(volatile unsigned int *)hex_value(argv[3]));
    add(line, " sleeping\n");
    print(line);
    int r;
    unsigned int t = timed_sleep(6000, &r);
    strcpy(line, "lt-ic: woke r");
    add_int(line, r);
    add(line, " ticks");
    add_int(line, t);
    add(line, "\n");
    print(line);
    int ok = r == 0 && t >= least_ticks(6000) && !registers_changed;
    print(ok ? "lt-ic: sleep ok\n" : "lt-ic: FAIL sleep\n");
    return !ok;
}

int main(int argc, char **argv)
{
    if (argc == 0)
    {
        return idle_boot();
    }
    const char *mode = argc > 1 ? argv[1] : "";
    if (strncmp(mode, "sleep", 6) == 0)
    {
        return sleeper(argc, argv);
    }
    if (strncmp(mode, "sleeppeer", 10) == 0)
    {
        return sleep_peer(argc, argv);
    }
    if (strncmp(mode, "idlechild", 10) == 0)
    {
        return idle_child(argc, argv);
    }
    if (strncmp(mode, "printc", 7) == 0)
    {
        return colours();
    }
    if (strncmp(mode, "strtok", 7) == 0)
    {
        return tokens();
    }
    if (strncmp(mode, "keys", 5) == 0)
    {
        return raw_keys();
    }
    if (strncmp(mode, "cursor", 7) == 0)
    {
        return cursor();
    }
    if (strncmp(mode, "readline", 9) == 0)
    {
        return edit_line();
    }
    print("libtest: usage libtest.elf printc|strtok|keys|cursor|readline|sleep\n");
    return 1;
}
