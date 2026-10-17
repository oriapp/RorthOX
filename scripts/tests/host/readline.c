/*
 * Host check for scripts/tests/readline.sh: the real rorthox_terminal_readline()
 * (programs/stdlib/src/rorthox.c), built with the host C compiler. The script builds rorthox.c
 * and string.c on their own and puts "user_" in front of every symbol (objcopy
 * --prefix-symbols), so nothing clashes with the host C library. The key syscall
 * (user_rorthox_getkey) plays a script of integer keys (bytes and RORTHOX_KEY_* values), with a
 * "no key yet" (0) before every key; the echo and cursor syscalls (user_rorthox_putchar,
 * user_rorthox_terminal_move_cursor) are recorded and drive a small 80 x 25 screen model that
 * writes, wraps and scrolls like the kernel terminal.
 *
 * Backspace at the start of the line must be ignored: not echoed (the echo would erase the
 * shell's prompt) and not stored (K26: it was stored as 0x08 and used up a place in the line).
 * The first ten cases check the exact echo for that: backspace + Enter; backspaces before a
 * line, with a line that exactly fills the buffer; a + 2 backspaces + b; echo off; a full
 * buffer with guard bytes around it; an ordinary erase; many backspaces into a small buffer;
 * Enter alone.
 *
 * The editing cases compare with a plain string model of the editing rules (insert at the
 * cursor, Left/Right/Home/End clamped to the line, Delete, Backspace, Enter, other special keys
 * and LF ignored, return when the buffer is full, at most 1023 characters when echoing) and,
 * when echoing, with the screen: the line's text from where it started, blank cells after it
 * (no stale character after a delete), the prompt and the rows above it unchanged (moved up
 * when the screen scrolled), the cursor never before the line's start or off the page, and
 * the cursor at the line's end when readline returns. Echo off must make no terminal call.
 *
 * Prints "readline host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "rorthox-config.h"

void user_rorthox_terminal_readline(char *out, int max, _Bool output_while_typing);

int strcmp(const char *a, const char *b);
void *memset(void *s, int c, unsigned long n);
unsigned long strlen(const char *s);

#define BS 0x08
#define ENTER 0x0d
#define GUARD 0xA5
#define BEFORE 16
#define AREA (2048 + 64)
#define UP RORTHOX_KEY_UP
#define DOWN RORTHOX_KEY_DOWN
#define LEFT RORTHOX_KEY_LEFT
#define RIGHT RORTHOX_KEY_RIGHT
#define HOME RORTHOX_KEY_HOME
#define END RORTHOX_KEY_END
#define INS RORTHOX_KEY_INSERT
#define DEL RORTHOX_KEY_DELETE
#define PGUP RORTHOX_KEY_PAGE_UP
#define PGDN RORTHOX_KEY_PAGE_DOWN

#define COLS 80
#define CELLS (COLS * 25)

static const int *keys;
static int next_key;
static int key_count;
static int zero_next;
static int overrun;
static char echo[256];
static int echo_count;
static int move_count;
static int move_delta;

// The screen model: cells, the cursor, where the line starts and how often the screen scrolled.
static unsigned char screen[CELLS];
static unsigned char above[CELLS];
static int cursor;
static int origin;
static int scrolls;
static int screen_bad;
static char screen_why[120];

static void screen_fail(const char *why)
{
    if (!screen_bad++)
    {
        int k = 0;
        for (; why[k] && k < (int)sizeof(screen_why) - 1; k++)
            screen_why[k] = why[k];
        screen_why[k] = 0;
    }
}

static void screen_put(unsigned char c)
{
    if (cursor < origin)
        screen_fail("wrote before the line's start");
    if (c == '\n')
        cursor = cursor - cursor % COLS + COLS;
    else if (c == BS)
    {
        screen_fail("echoed a backspace");
        if (cursor > 0)
            screen[--cursor] = ' ';
        return;
    }
    else
        screen[cursor++] = c;
    if (cursor >= CELLS)
    {
        for (int i = 0; i < CELLS - COLS; i++)
            screen[i] = screen[i + COLS];
        for (int i = CELLS - COLS; i < CELLS; i++)
            screen[i] = ' ';
        cursor -= COLS;
        origin -= COLS;
        scrolls++;
    }
}

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
    return keys[next_key++];
}

void user_rorthox_putchar(char c)
{
    if (echo_count < (int)sizeof(echo) - 1)
    {
        echo[echo_count] = c;
    }
    echo_count++;
    screen_put((unsigned char)c);
}

int user_rorthox_terminal_move_cursor(int delta)
{
    move_count++;
    move_delta += delta;
    // The kernel refuses moves off the page; readline must never need one.
    if (delta < -cursor || delta > CELLS - 1 - cursor)
    {
        screen_fail("cursor move off the page");
        return -1;
    }
    cursor += delta;
    if (cursor < origin)
        screen_fail("cursor moved before the line's start");
    return 0;
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

// A fresh screen: rows of known text above, "> " right before (col,row), blanks from there on.
static void screen_setup(int col, int row)
{
    for (int i = 0; i < CELLS; i++)
        screen[i] = 'A' + (i * 7 + i / COLS) % 26;
    origin = cursor = row * COLS + col;
    if (origin >= 1)
        screen[origin - 1] = ' ';
    if (origin >= 2)
        screen[origin - 2] = '>';
    for (int i = origin; i < CELLS; i++)
        screen[i] = ' ';
    for (int i = 0; i < CELLS; i++)
        above[i] = screen[i];
    scrolls = 0;
    screen_bad = 0;
    screen_why[0] = 0;
}

// The screen after the read: everything before the line as it was (moved up by the scrolls),
// the line's text, then blanks; the cursor right after the line.
static void screen_check(const char *line, int len)
{
    int start = origin + scrolls * COLS;
    if (origin < 0)
    {
        screen_fail("the line's start scrolled off the screen");
        return;
    }
    for (int i = 0; i < origin; i++)
    {
        if (screen[i] != above[i + scrolls * COLS] && i + scrolls * COLS < start)
        {
            screen_fail("the prompt or a row above the line changed");
            break;
        }
    }
    for (int i = 0; i < len; i++)
    {
        if (origin + i >= CELLS || screen[origin + i] != (unsigned char)line[i])
        {
            screen_fail("the line on the screen differs");
            break;
        }
    }
    for (int i = origin + len; i < CELLS; i++)
    {
        if (screen[i] != ' ')
        {
            screen_fail("a character is left after the line's end");
            break;
        }
    }
    if (cursor != origin + len)
        screen_fail("the cursor is not at the line's end");
}

static int cases;
static int failed;

static void show(const char *name, const char *s, int n)
{
    printf("%s \"", name);
    for (int i = 0; i < n && i < 90; i++)
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
    printf("%s\"", n > 90 ? "..." : "");
}

static unsigned char area[AREA];

// Reads one line: <script> (<script_len> keys) into a buffer of <max> starting at (col,row).
static char *read_line(const int *script, int script_len, int max, int output, int col, int row)
{
    char *out = (char *)area + BEFORE;
    keys = script;
    key_count = script_len;
    next_key = 0;
    zero_next = 0;
    overrun = 0;
    echo_count = 0;
    move_count = 0;
    move_delta = 0;
    memset(echo, 0, sizeof(echo));
    memset(area, GUARD, sizeof(area));
    screen_setup(col, row);
    user_rorthox_terminal_readline(out, max, output);
    return out;
}

// Nothing written outside out[0 .. max-1].
static int guards_ok(int max)
{
    for (int i = 0; i < AREA; i++)
    {
        if ((i < BEFORE || i >= BEFORE + max) && area[i] != GUARD)
            return 0;
    }
    return 1;
}

static void report(const char *what, int ok, const char *out, int stored, int want_used)
{
    cases++;
    if (!ok)
    {
        failed++;
        printf("  FAIL %s: ", what);
        show("stored", out, stored);
        printf(", ");
        show("echoed", echo, echo_count < (int)sizeof(echo) ? echo_count : (int)sizeof(echo) - 1);
        printf(", %d keys taken (expected %d)%s%s%s\n", next_key, want_used, overrun ? ", ran out of keys" : "",
               screen_bad ? ", screen: " : "", screen_why);
    }
}

// One case with the exact echo: expect <want> stored, <want_echo> echoed and <want_used> keys taken.
static void run(const char *what, const int *script, int script_len, int max, int output, const char *want,
                const char *want_echo, int want_used, int want_moves)
{
    char *out = read_line(script, script_len, max, output, 2, 3);

    int ok = !overrun && next_key == want_used;
    ok = ok && move_count == want_moves && move_delta == -want_moves;
    // The line and its terminator, and nothing written outside out[0 .. max-1].
    int len = strlen(want);
    for (int i = 0; i < len + 1; i++)
    {
        ok = ok && out[i] == want[i];
    }
    ok = ok && guards_ok(max);
    ok = ok && echo_count == (int)strlen(want_echo) && strcmp(echo, want_echo) == 0;
    if (output)
        screen_check(want, len);
    ok = ok && !screen_bad;
    report(what, ok, out, max, want_used);
}

// The editing rules as a plain string model: what the line must be and how many keys it takes.
static int model(const int *script, int script_len, int max, int output, char *line, int *used)
{
    int cap = max - 1, len = 0, pos = 0, k = 0;
    if (output && cap > 1023)
        cap = 1023;
    while (len < cap && k < script_len)
    {
        int key = script[k++];
        if (key == ENTER)
            break;
        if (key == LEFT && pos > 0)
            pos--;
        else if (key == RIGHT && pos < len)
            pos++;
        else if (key == HOME)
            pos = 0;
        else if (key == END)
            pos = len;
        else if ((key == BS && pos > 0) || (key == DEL && pos < len))
        {
            if (key == BS)
                pos--;
            for (int i = pos; i < len - 1; i++)
                line[i] = line[i + 1];
            len--;
        }
        else if (key >= 1 && key <= 255 && key != BS && key != '\n')
        {
            for (int i = len; i > pos; i--)
                line[i] = line[i - 1];
            line[pos++] = (char)key;
            len++;
        }
    }
    line[len] = 0;
    *used = k;
    return len;
}

// One editing case, compared with the model, the guards and (echo on) the screen; echo off
// must make no terminal call. (col,row): where the line starts.
static void edit(const char *what, const int *script, int script_len, int max, int output, int col, int row)
{
    static char want[2100];
    int want_used;
    int len = model(script, script_len, max, output, want, &want_used);
    char *out = read_line(script, script_len, max, output, col, row);

    int ok = !overrun && next_key == want_used && guards_ok(max);
    for (int i = 0; i < len + 1; i++)
    {
        ok = ok && out[i] == want[i];
    }
    if (output)
        screen_check(want, len);
    else
        ok = ok && echo_count == 0 && move_count == 0;
    ok = ok && !screen_bad;
    report(what, ok, out, len + 1, want_used);
}

#define KEYS(...) (int[]){__VA_ARGS__}, (int)(sizeof((int[]){__VA_ARGS__}) / sizeof(int))
#define COMMAND 'b', 'l', 'a', 'n', 'k', '.', 'e', 'l', 'f'

int main(void)
{
    static int many[2200];

    run("backspace, Enter", KEYS(BS, ENTER), 16, 1, "", "", 2, 0);
    run("3 backspaces, then a line", KEYS(BS, BS, BS, COMMAND, ' ', 'r', 'l', '1', ENTER), 64, 1,
        "blank.elf rl1", "blank.elf rl1", 17, 0);
    run("3 backspaces, then a line that fills the buffer", KEYS(BS, BS, BS, COMMAND, ENTER), 10, 1,
        "blank.elf", "blank.elf", 12, 0);
    run("a, 2 backspaces, b", KEYS('a', BS, BS, 'b', ENTER), 16, 1, "b", "a b", 5, 2);
    run("a, 3 backspaces, b", KEYS('a', BS, BS, BS, 'b', ENTER), 16, 1, "b", "a b", 6, 2);
    run("echo off", KEYS(BS, 'x', BS, BS, 'y', ENTER), 16, 0, "y", "", 6, 0);
    // max 4: three characters and the terminator; the 'd' stays unread.
    run("full buffer", KEYS(BS, BS, 'a', 'b', 'c', 'd', ENTER), 4, 1, "abc", "abc", 5, 0);
    run("ordinary erase", KEYS('a', 'b', BS, 'c', ENTER), 16, 1, "ac", "ab c", 5, 2);
    run("Enter alone", KEYS(ENTER), 16, 1, "", "", 1, 0);

    for (int i = 0; i < 150; i++)
        many[i] = BS;
    many[150] = 'x';
    many[151] = 'y';
    many[152] = ENTER;
    run("150 backspaces into a buffer of 3", many, 153, 3, 1, "xy", "xy", 152, 0);

    edit("insert at the start, the end and the middle",
         KEYS('l', 'a', 'n', 'k', HOME, 'b', END, '.', 'e', 'f', LEFT, 'l', ENTER), 64, 1, 2, 3);
    edit("Left and Right stop at both ends",
         KEYS(LEFT, LEFT, LEFT, RIGHT, RIGHT, 'a', 'b', RIGHT, RIGHT, RIGHT, LEFT, LEFT, LEFT, LEFT, LEFT, 'x', ENTER),
         64, 1, 2, 3);
    edit("Home and End on an empty line", KEYS(HOME, END, HOME, 'q', END, HOME, END, 'r', ENTER), 64, 1, 2, 3);
    edit("Delete at the start, in the middle and at the end",
         KEYS('a', 'b', 'c', 'd', HOME, DEL, RIGHT, DEL, END, DEL, DEL, ENTER), 64, 1, 2, 3);
    edit("Backspace in the middle and at the start",
         KEYS('a', 'b', 'c', LEFT, BS, HOME, BS, BS, 'x', ENTER), 64, 1, 2, 3);
    edit("Enter with the cursor in the middle", KEYS('a', 'b', 'c', HOME, RIGHT, ENTER), 64, 1, 2, 3);
    edit("an insert in the middle fills the buffer", KEYS('a', 'b', LEFT, 'x', ENTER, 'z'), 4, 1, 2, 3);
    edit("editing with echo off",
         KEYS('a', 'c', LEFT, 'b', HOME, DEL, END, 'd', LEFT, LEFT, BS, ENTER), 64, 0, 2, 3);
    edit("other special keys and LF are ignored",
         KEYS('a', UP, DOWN, INS, PGUP, PGDN, 0x10a, 0x1ff, 0xffff, '\n', LEFT, UP, 'b', ENTER), 64, 1, 2, 3);
    edit("bytes 0x80 and 0xff are characters", KEYS(0x80, 'a', 0xff, LEFT, 0x80, HOME, 0xfe, ENTER), 64, 1, 2, 3);
    edit("tab and escape are one cell each", KEYS('a', '\t', 0x1b, HOME, DEL, ENTER), 64, 1, 2, 3);

    // max 0 and below: nothing read, nothing written. max 1: only the terminator.
    for (int max = -1; max <= 1; max++)
    {
        char *out = read_line(KEYS('a', ENTER), max, 1, 2, 3);
        int ok = next_key == 0 && echo_count == 0 && move_count == 0 && guards_ok(max > 0 ? max : 0);
        if (max == 1)
            ok = ok && out[0] == 0;
        report(max < 0 ? "max -1" : max == 0 ? "max 0" : "max 1", ok, out, 1, 0);
    }

    // Starts at columns 0, 77 and 79: the line wraps, Home goes back over the row edge.
    static const int starts[][2] = { { 0, 5 }, { 77, 5 }, { 79, 5 }, { 77, 24 }, { 79, 24 } };
    for (unsigned s = 0; s < sizeof(starts) / sizeof(starts[0]); s++)
    {
        const char *name[] = { "start in column 0", "start in column 77", "start in column 79",
                               "start in column 77 of the bottom row (scrolls)",
                               "start in column 79 of the bottom row (scrolls)" };
        edit(name[s], KEYS('a', 'b', 'c', 'd', 'e', 'f', HOME, 'x', END, BS, LEFT, LEFT, LEFT, DEL, 'y', LEFT,
                           BS, END, 'z', ENTER),
             64, 1, starts[s][0], starts[s][1]);
    }

    // A long line from the bottom row, edited at the start and the end while it scrolls.
    for (int i = 0; i < 200; i++)
        many[i] = 'a' + i % 26;
    int n = 200;
    int tail[] = { HOME, 'X', 'Y', DEL, END, LEFT, LEFT, LEFT, DEL, 'Z', BS, HOME, RIGHT, RIGHT, BS, ENTER };
    for (unsigned i = 0; i < sizeof(tail) / sizeof(tail[0]); i++)
        many[n++] = tail[i];
    edit("200 characters from the bottom row, edits at both ends", many, n, 512, 1, 30, 24);

    // Echo stops at 1023 characters even with a bigger buffer; the read returns full.
    for (int i = 0; i < 1100; i++)
        many[i] = '0' + i % 10;
    edit("1023 echoed characters into a buffer of 2048", many, 1100, 2048, 1, 79, 24);
    many[1100] = ENTER;
    edit("1100 characters without echo into a buffer of 2048", many, 1101, 2048, 0, 0, 0);

    // A 1023-character line edited at its start and end before the last characters fill it.
    n = 0;
    for (int i = 0; i < 1015; i++)
        many[n++] = 'a' + i % 26;
    int full[] = { HOME, 'S', DEL, DEL, END, LEFT, LEFT, 'M', RIGHT, DEL, HOME, RIGHT, BS, END };
    for (unsigned i = 0; i < sizeof(full) / sizeof(full[0]); i++)
        many[n++] = full[i];
    for (int i = 0; i < 20; i++)
        many[n++] = '0' + i % 10;
    edit("a full 1023-character line edited at both ends", many, n, 1024, 1, 2, 24);

    printf("readline host check: %d of %d cases failed\n", failed, cases);
    fflush(0);
    return failed ? 1 : 0;
}
