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
 * libtest.elf sleep
 *     Sleep (syscall 12), for scripts/tests/sleep.sh. Times its sleeps with the tick syscall (19).
 *     Prints "libtest sleep: armed" and waits for a key.
 *     Then "lt-s: zero 0 negative -2 high -2" (sleep(0), sleep(-1), sleep(0x80000000)), two
 *     sleep(50) with their results and ticks, "lt-s: short sleeps ok" if both returned 0 after at
 *     least 7 ticks, starts "libtest.elf sleeppeer" and runs sleep(1000), then calls
 *     lt_sleeper_woke() (a debugger stop for sleep.sh): "lt-s: slept r <r>
 *     ticks <n> from <t0> to <t1>" and "lt-s: long sleep ok" for r 0 and n >= 102. Every sleep
 *     runs with the register and stack markers of printc; "lt-s: ebx esi edi ebp esp and stack
 *     kept". Ends with "libtest sleep: done" (or FAIL lines and "libtest sleep: FAIL").
 *
 * libtest.elf sleeppeer
 *     Started by sleep. Runs for PEER_TICKS ticks (by the tick syscall), which spans the sleeper's
 *     sleep(1000), and calls lt_peer_sample(n, tick) once for every new tick it sees (n = 1, 2,
 *     ...). It knows nothing about the sleeper: sleep.sh breaks at lt_peer_sample and looks at
 *     the sleeper's task from the host, then acknowledges the sample by writing n into
 *     lt_peer_ack. At the end it calls lt_peer_done(samples) and prints "lt-p: ran from <first>
 *     to <last> samples <n> acknowledged <a>".
 *
 * Started by the kernel as shell.elf (no arguments at all), for scripts/tests/sleep-idle.sh
 *     Prints "libtest idle: armed" and "lt-i: sleeping", sleeps 5000 ms as the only task (timed
 *     with the tick syscall, like every sleep here), reads one key and prints
 *     "lt-i: woke r <r> ticks <n> key <key>" and
 *     "lt-i: first sleep ok" (r 0, n >= 502, key 'k'). Then starts "libtest.elf idlechild",
 *     prints "lt-i: second sleeping", sleeps 3000 ms, "lt-i: second sleep ok" (n >= 302) and
 *     "libtest idle: done", and sleeps on forever (the last process must not exit).
 *
 * libtest.elf idlechild
 *     Prints "lt-ic: sleeping", sleeps 6000 ms, prints its result and ticks and
 *     "lt-ic: sleep ok" (r 0, n >= 602), and ends.
 *
 * libtest.elf clock
 *     The tick syscall (19), for scripts/tests/clock.sh. Every rorthox_get_ticks() call runs with
 *     the register and stack markers of printc. Prints "\nlt-t: start <ticks>", then:
 *     "lt-t: sleeps ok" if each of three sleep(50) took at least 7 and at most 100 ticks by
 *     rorthox_get_ticks(); "lt-t: advanced 5 times" if a busy loop saw the value go up 5 times,
 *     each time by 1..100, within 2000000 calls; "lt-t: garbage esp ok" if int 0x80 with eax 19
 *     and esp set to 0, 0xdeadbeef, 0xfffffffc or 0x3ff001 (none of them a stack the program owns),
 *     repeated until at least 30 ticks have passed ("lt-t: garbage esp rounds <n> ticks <t>"),
 *     always returned a value between the calls before and after it and gave the same esp back; and
 *     "lt-t: ebx esi edi ebp esp and stack kept". Ends with "libtest clock: done" (or FAIL lines
 *     and "libtest clock: FAIL").
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
    unsigned int start = rorthox_get_ticks();
    *result = sleep_checked(milliseconds);
    return rorthox_get_ticks() - start;
}

// Ticks a sleep must take at least: ceil(ms / 10) + 2 at 100 Hz.
static unsigned int least_ticks(int milliseconds)
{
    return (milliseconds + 9) / 10 + 2;
}

// sleep.sh breaks here (by symbol) once the sleeper's sleep(1000) has returned and checks from
// the host that the sleeper runs again: runnable, past its deadline, in its own page directory.
__attribute__((noinline, noipa)) void lt_sleeper_woke(void)
{
    __asm__ volatile("" : : : "memory");
}

// libtest.elf sleep: see the top of the file.
static int sleeper(int argc, char **argv)
{
    char line[160];
    int failed = 0;
    if (argc != 2)
    {
        print("libtest sleep: usage libtest.elf sleep\n");
        return 1;
    }
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

    // The peer runs while this task sleeps; sleep.sh watches both from the host.
    rorthox_system_run("libtest.elf sleeppeer");

    int r;
    unsigned int start = rorthox_get_ticks();
    r = sleep_checked(1000);
    lt_sleeper_woke();
    unsigned int end = rorthox_get_ticks();
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

// libtest.elf sleeppeer: started by the sleeper, see the top of the file. Long enough to span
// the sleeper's sleep(1000), which starts within a few ticks of this program.
#define PEER_TICKS 250

// The number of the last sample sleep.sh's debugger has looked at, written by the debugger.
volatile unsigned int lt_peer_ack;

// sleep.sh breaks here (by symbol) once per new tick the peer sees and reads the sleeper's task
// while the VM is stopped. Kept out of line and out of interprocedural optimisation, so it has
// one address and its arguments are on the stack at the breakpoint.
__attribute__((noinline, noipa)) void lt_peer_sample(unsigned int number, unsigned int tick)
{
    __asm__ volatile("" : : "r"(number), "r"(tick) : "memory");
}

// sleep.sh breaks here once at the end: no samples follow.
__attribute__((noinline, noipa)) void lt_peer_done(unsigned int samples)
{
    __asm__ volatile("" : : "r"(samples) : "memory");
}

static int sleep_peer(int argc, char **argv)
{
    char line[160];
    if (argc != 2)
    {
        return 1;
    }
    unsigned int first = rorthox_get_ticks();
    unsigned int last = first;
    unsigned int samples = 0, acknowledged = 0;
    while (last - first < PEER_TICKS)
    {
        unsigned int now = rorthox_get_ticks();
        if (now == last)
        {
            continue;
        }
        last = now;
        samples++;
        lt_peer_sample(samples, now);
        acknowledged += lt_peer_ack == samples;
    }
    lt_peer_done(samples);
    strcpy(line, "lt-p: ran from");
    add_int(line, first);
    add(line, " to");
    add_int(line, last);
    add(line, " samples");
    add_int(line, samples);
    add(line, " acknowledged");
    add_int(line, acknowledged);
    add(line, "\n");
    print(line);
    return 0;
}

// Started by the kernel as shell.elf (no arguments): see the top of the file.
static int idle_boot(void)
{
    char line[160];
    int failed = 0;
    print("libtest idle: armed\n");

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
    rorthox_system_run("libtest.elf idlechild");
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

// libtest.elf idlechild: started by idle_boot.
static int idle_child(int argc, char **argv)
{
    char line[160];
    if (argc != 2)
    {
        return 1;
    }
    print("lt-ic: sleeping\n");
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

// ---- ticks (syscall 19) ----

static unsigned int ticks_checked(void)
{
    return (unsigned int)call_checked(rorthox_get_ticks, 0, 0);
}

// int 0x80 with eax = 19 and esp = bad. The value in eax, and the esp the kernel gave back in
// *esp_back. esi keeps the real esp: the syscall must give it back unchanged.
static unsigned int ticks_with_esp(unsigned int bad, unsigned int *esp_back)
{
    unsigned int result, back;
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "mov %%edx, %%esp\n\t"
                     "mov %[number], %%eax\n\t"
                     "int %[vector]\n\t"
                     "mov %%esp, %%edx\n\t"
                     "mov %%esi, %%esp"
                     : "=&a"(result), "=d"(back)
                     : "d"(bad), [number] "i"(SYSCALL_GET_TICKS), [vector] "i"(RORTHOX_SYSCALL_VECTOR)
                     : "esi", "memory", "cc");
    *esp_back = back;
    return result;
}

// libtest.elf clock: see the top of the file.
static int clock_ticks(void)
{
    char line[160];
    int failed = 0;
    unsigned int start = ticks_checked();
    strcpy(line, "\nlt-t: start");
    add_hex(line, start);
    add(line, "\n");
    print(line);

    // Each sleep(50) waits for at least 7 delivered ticks.
    int ok = 1;
    for (int i = 0; i < 3; i++)
    {
        unsigned int before = ticks_checked();
        int r = sleep(50);
        unsigned int took = ticks_checked() - before;
        if (r != 0 || took < least_ticks(50) || took > 100)
        {
            fail("clock", "sleep(50) ticks", 50, (int)took, (int)least_ticks(50));
            ok = 0;
        }
    }
    print(ok ? "lt-t: sleeps ok\n" : "lt-t: FAIL sleeps\n");
    failed |= !ok;

    // Busy loop: the value goes up as ticks are delivered, never down, never by much.
    unsigned int last = ticks_checked(), calls = 0;
    int changes = 0;
    ok = 1;
    while (changes < 5 && calls < 2000000)
    {
        unsigned int now = ticks_checked();
        calls++;
        if (now != last)
        {
            if (now - last > 100)
            {
                fail("clock", "jump from", last, (int)now, (int)(last + 1u));
                ok = 0;
            }
            last = now;
            changes++;
        }
    }
    strcpy(line, ok && changes == 5 ? "lt-t: advanced 5 times\n" : "lt-t: FAIL advanced");
    if (!(ok && changes == 5))
    {
        add_int(line, changes);
        add(line, " times in");
        add_int(line, (int)calls);
        add(line, " calls\n");
        ok = 0;
    }
    print(line);
    failed |= !ok;

    // A garbage esp: the handler reads nothing from the user stack. Repeated until at least 30
    // ticks have passed (at most 100000 rounds), so timer IRQs also come in while esp is garbage
    // (right after the iret, or between the mov and the int). Stops at the first wrong value.
    static const unsigned int bad[] = { 0, 0xdeadbeef, 0xfffffffc, 0x3ff001 };
    ok = 1;
    unsigned int first = ticks_checked(), rounds = 0, passed = 0;
    while (ok && passed < 30 && rounds < 100000)
    {
        for (unsigned i = 0; ok && i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            unsigned int esp_back;
            unsigned int before = ticks_checked();
            unsigned int got = ticks_with_esp(bad[i], &esp_back);
            unsigned int after = ticks_checked();
            if (before == 0 || got - before > after - before || esp_back != bad[i])
            {
                strcpy(line, "libtest clock: FAIL garbage esp");
                add_hex(line, bad[i]);
                add(line, " got");
                add_hex(line, got);
                add(line, " between");
                add_hex(line, before);
                add_hex(line, after);
                add(line, " esp back");
                add_hex(line, esp_back);
                add(line, "\n");
                print(line);
                ok = 0;
            }
        }
        rounds++;
        passed = ticks_checked() - first;
    }
    strcpy(line, "lt-t: garbage esp rounds");
    add_int(line, (int)rounds);
    add(line, " ticks");
    add_int(line, (int)passed);
    add(line, "\n");
    print(line);
    ok &= passed >= 30;
    print(ok ? "lt-t: garbage esp ok\n" : "lt-t: FAIL garbage esp\n");
    failed |= !ok;

    if (registers_changed)
    {
        fail("clock", "registers changed, mask", registers_changed, registers_changed, 0);
        failed = 1;
    }
    else
    {
        print("lt-t: ebx esi edi ebp esp and stack kept\n");
    }
    print(failed ? "libtest clock: FAIL\n" : "libtest clock: done\n");
    return failed;
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
    if (strncmp(mode, "clock", 6) == 0)
    {
        return clock_ticks();
    }
    print("libtest: usage libtest.elf printc|strtok|keys|cursor|readline|sleep|clock\n");
    return 1;
}
