/*
 * Host check for scripts/tests/tty-backspace.sh and scripts/tests/tty-scroll.sh: the real
 * kernel/arch/i686/tty.c, built with the host C compiler, writing to a VGA text page mapped at
 * its real address 0xB8000 (one 4 KiB page; the pages below and above are not mapped, so a store
 * outside the page stops the case with SIGSEGV). The 96 bytes after the 80 x 25 screen
 * (0xB8FA0-0xB8FFF) are filled with a pattern that must stay. outb() is a stub that keeps the
 * cursor position written to the CRT controller (0x3D4/0x3D5).
 *
 * Every case runs in its own process, starts from terminal_initialize() with every screen cell
 * set to a known character/colour, and compares all cells, the row, the column and the cursor
 * with a small model of what the terminal must do.
 *
 * Group "backspace" (K24): backspace at (0,0), in the middle of a row, at column 0 of a later row
 * (goes to column 79 of the row above), many times across rows, on the last row, through
 * terminal_backspace() and through print("\b"), and the next character lands on the erased cell.
 *
 * Group "scroll" (K7): the screen is 80 x 25; print, printc, terminal_writechar and printf
 * (literal text, newlines, %i, an unknown format character) scroll at the bottom instead of
 * writing below row 24; scrolled rows keep both bytes of every cell and the new bottom row is
 * cleared to spaces in the terminal colour; terminal_putchar ignores coordinates off the screen;
 * a long mixed run of output never leaves the screen page.
 *
 * Group "cursor": terminal_move_cursor() within a row, across rows, to the first and last cell,
 * one past them, INT_MIN/INT_MAX and other off-page deltas, and on a scrolled screen. A move
 * changes no cell; a valid one sets row, column and the CRT cursor, a refused one (-EINVARG)
 * changes none of them; the next output lands at the new place.
 *
 * Usage: tty-screen backspace|scroll|cursor
 * Prints "tty-screen <group> host check: <failed> of <cases> cases failed".
 */
#include <stdint.h>
#include <stdarg.h>

// tty.c has its own printf and vprintf: rename them so the check can use the host printf.
#define printf kernel_printf
#define vprintf kernel_vprintf
#include "arch/i686/tty.c"
#undef printf
#undef vprintf

#include "host.h"

void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
int munmap(void *addr, unsigned long length);
int strcmp(const char *a, const char *b);

#define VGA_ADDRESS 0xB8000UL
#define PAGE 4096UL
#define COLS 80
#define CELLS (COLS * 25)
#define SCREEN_BYTES (CELLS * 2)
#define GUARD 0xCC
#define WHITE 0x0F
#define CLEAR (0x20 | (WHITE << 8))

static volatile uint16_t *const vga = (volatile uint16_t *)VGA_ADDRESS;
static volatile uint8_t *const vga_bytes = (volatile uint8_t *)VGA_ADDRESS;

// The CRT controller: the register selected through 0x3D4 and the cursor location it holds.
static int crtc_index;
static int cursor_reg = -1;

void outb(unsigned short port, unsigned char val)
{
    if (port == 0x3D4)
    {
        crtc_index = val;
    }
    else if (port == 0x3D5)
    {
        if (cursor_reg < 0)
        {
            cursor_reg = 0;
        }
        if (crtc_index == 0x0F)
        {
            cursor_reg = (cursor_reg & 0xFF00) | val;
        }
        else if (crtc_index == 0x0E)
        {
            cursor_reg = (cursor_reg & 0x00FF) | (val << 8);
        }
    }
}

unsigned char insb(unsigned short port)
{
    return 0;
}

// The model: what the screen, row and column must be.
static uint16_t want[CELLS];
static int want_row, want_col, height;

static void model_scroll(void)
{
    for (int i = 0; i < CELLS - COLS; i++)
    {
        want[i] = want[i + COLS];
    }
    for (int i = CELLS - COLS; i < CELLS; i++)
    {
        want[i] = CLEAR;
    }
    want_row = 24;
}

static void model_newline(void)
{
    want_col = 0;
    want_row++;
    if (want_row >= 25)
    {
        model_scroll();
    }
}

static void model_put(char c, int attr)
{
    want[want_row * COLS + want_col] = (uint8_t)c | (attr << 8);
    want_col++;
    if (want_col >= COLS)
    {
        model_newline();
    }
}

static void model_backspace(void)
{
    if (want_row == 0 && want_col == 0)
    {
        return;
    }
    if (want_col == 0)
    {
        want_row--;
        want_col = COLS - 1;
    }
    else
    {
        want_col--;
    }
    want[want_row * COLS + want_col] = ' ' | (WHITE << 8);
}

// Text through print(): '\n' and '\b' are interpreted, everything else is a white character.
static void model_print(const char *s, int attr)
{
    for (; *s; s++)
    {
        if (*s == '\n')
        {
            model_newline();
        }
        else if (*s == 0x08)
        {
            model_backspace();
        }
        else
        {
            model_put(*s, attr);
        }
    }
}

static void set_position(int col, int row)
{
    terminal_col = col;
    terminal_row = row;
    want_col = col;
    want_row = row;
}

// A fresh terminal: terminal_initialize(black), then every cell a known character and colour,
// the bytes after the screen a guard pattern, and the model in the same state.
static void setup(void)
{
    terminal_initialize(VGA_COLOR_BLACK);
    for (int i = 0; i < CELLS; i++)
    {
        uint16_t v = (uint16_t)('a' + i % 26) | (uint16_t)((0x10 + i % 7) << 8);
        vga[i] = v;
        want[i] = v;
    }
    for (unsigned long i = SCREEN_BYTES; i < PAGE; i++)
    {
        vga_bytes[i] = GUARD;
    }
    set_position(terminal_col, terminal_row);
    cursor_reg = -1;
}

static int problems;

#define BAD(...) do { if (problems++ < 8) { printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

// Compare everything with the model. cursor: also check the CRT cursor.
static void verify(const char *what, int cursor)
{
    for (int i = 0; i < CELLS; i++)
    {
        if (vga[i] != want[i])
        {
            BAD("%s: cell (%d,%d) is %c/0x%02x, expected %c/0x%02x", what, i % COLS, i / COLS,
                vga[i] & 0xFF, vga[i] >> 8, want[i] & 0xFF, want[i] >> 8);
            break;
        }
    }
    for (unsigned long i = SCREEN_BYTES; i < PAGE; i++)
    {
        if (vga_bytes[i] != GUARD)
        {
            BAD("%s: byte 0x%lx after the screen was written", what, VGA_ADDRESS + i);
            break;
        }
    }
    if (terminal_col != want_col || terminal_row != want_row)
    {
        BAD("%s: position (%d,%d), expected (%d,%d)", what, terminal_col, terminal_row, want_col, want_row);
    }
    if (cursor && cursor_reg != want_row * COLS + want_col)
    {
        BAD("%s: VGA cursor at %d, expected %d (%d,%d)", what, cursor_reg, want_row * COLS + want_col,
            want_col, want_row);
    }
}

// --- group backspace (K24) ---

static void bs_origin(void)
{
    setup();
    set_position(0, 0);
    terminal_writechar(0x08, WHITE);
    verify("backspace at (0,0)", 0);
}

static void bs_middle(void)
{
    setup();
    set_position(10, 3);
    terminal_writechar(0x08, WHITE);
    model_backspace();
    verify("backspace at (10,3)", 1);
    terminal_writechar('Y', 0x0E);
    model_put('Y', 0x0E);
    verify("character after backspace at (10,3)", 1);
}

static void bs_row_start(void)
{
    setup();
    set_position(0, 1);
    terminal_writechar(0x08, WHITE);
    model_backspace();
    verify("backspace at (0,1)", 1);
}

static void bs_row_start_then_char(void)
{
    setup();
    set_position(0, 2);
    terminal_writechar(0x08, WHITE);
    model_backspace();
    verify("backspace at (0,2)", 1);
    terminal_writechar('X', 0x0E);
    model_put('X', 0x0E);
    verify("character after backspace at (0,2)", 1);
}

static void bs_direct(void)
{
    setup();
    set_position(0, 6);
    terminal_backspace();
    model_backspace();
    verify("terminal_backspace() at (0,6)", 1);
}

static void bs_print(void)
{
    // The syscall path: one print() with a full row, a backspace and a replacement.
    setup();
    set_position(0, 7);
    char line[COLS + 3];
    for (int i = 0; i < COLS; i++)
    {
        line[i] = i == COLS - 1 ? 'Z' : '0' + i % 10;
    }
    line[COLS] = 0x08;
    line[COLS + 1] = 'R';
    line[COLS + 2] = 0;
    print(line);
    model_print(line, WHITE);
    verify("print(80 characters, backspace, R) from (0,7)", 1);
    if ((vga[7 * COLS + COLS - 1] & 0xFF) != 'R' || want_row != 8 || want_col != 0)
    {
        BAD("print(80 characters, backspace, R): R is not at (79,7)");
    }
}

static void bs_across_rows(void)
{
    setup();
    set_position(3, 5);
    for (int i = 0; i < 84; i++)
    {
        terminal_writechar(0x08, WHITE);
        model_backspace();
        verify("84 backspaces from (3,5)", 1);
        if (problems)
        {
            printf("  (after backspace %d)\n", i + 1);
            return;
        }
    }
    if (want_col != COLS - 1 || want_row != 3)
    {
        BAD("84 backspaces from (3,5) should end at (79,3)");
    }
}

static void bs_to_origin(void)
{
    setup();
    set_position(5, 2);
    for (int i = 0; i < 2 * COLS + 5 + 3; i++)
    {
        terminal_writechar(0x08, WHITE);
        model_backspace();
    }
    verify("backspaces from (5,2) past (0,0)", 1);
}

static void bs_last_row(void)
{
    setup();
    set_position(0, height - 1);
    terminal_writechar(0x08, WHITE);
    model_backspace();
    verify("backspace at column 0 of the last row", 1);
    set_position(5, height - 1);
    terminal_writechar(0x08, WHITE);
    model_backspace();
    verify("backspace at (5, last row)", 1);
    terminal_writechar('L', WHITE);
    model_put('L', WHITE);
    verify("character after backspace on the last row", 1);
}

// --- group scroll (K7) ---

static void sc_size(void)
{
    if (VGA_WIDTH != 80 || VGA_HEIGHT != 25)
    {
        BAD("the terminal is %d x %d, expected the full 80 x 25 text screen", VGA_WIDTH, VGA_HEIGHT);
    }
}

static int numbered(char *out, const char *what, int n, int len)
{
    int k = 0;
    for (const char *p = what; *p; p++)
    {
        out[k++] = *p;
    }
    out[k++] = '0' + n / 100 % 10;
    out[k++] = '0' + n / 10 % 10;
    out[k++] = '0' + n % 10;
    while (k < len)
    {
        out[k++] = 'a' + (n + k) % 26;
    }
    out[k] = 0;
    return k;
}

static void sc_print_lines(void)
{
    setup();
    char line[100];
    for (int n = 1; n <= 64; n++)
    {
        numbered(line, "line ", n, 10 + n % 50);
        line[10 + n % 50] = '\n';
        line[11 + n % 50] = 0;
        print(line);
        model_print(line, WHITE);
        verify("print, 64 lines", 1);
        if (problems)
        {
            printf("  (after line %d)\n", n);
            return;
        }
    }
    print("end");
    model_print("end", WHITE);
    verify("print, 64 lines and an end marker", 1);
    if (want_row != 24 || want_col != 3)
    {
        BAD("model: end marker not on row 24");
    }
}

static void sc_wrap(void)
{
    setup();
    char block[COLS + 1];
    for (int n = 1; n <= 40; n++)
    {
        numbered(block, "block ", n, COLS);
        print(block);
        model_print(block, WHITE);
        verify("print, 40 blocks of 80 characters", 1);
        if (problems)
        {
            printf("  (after block %d)\n", n);
            return;
        }
    }
}

static void sc_colours(void)
{
    // Both bytes of every cell move: each line has its own colour.
    setup();
    char line[100];
    for (int n = 1; n <= 40; n++)
    {
        numbered(line, "colour ", n, 20 + n);
        line[20 + n] = '\n';
        line[21 + n] = 0;
        printc(line, 1 + n % 15);
        model_print(line, 1 + n % 15);
        verify("printc, 40 lines in 15 colours", 1);
        if (problems)
        {
            printf("  (after line %d)\n", n);
            return;
        }
    }
}

static void sc_putchar(void)
{
    // The putchar syscall calls terminal_writechar for each character.
    setup();
    for (int i = 0; i < 30 * COLS + 7; i++)
    {
        char c = i % 97 == 96 ? '\n' : 'A' + i % 26;
        terminal_writechar(c, WHITE);
        if (c == '\n')
        {
            model_newline();
        }
        else
        {
            model_put(c, WHITE);
        }
        verify("terminal_writechar, 2407 characters", 1);
        if (problems)
        {
            printf("  (after character %d)\n", i + 1);
            return;
        }
    }
}

static void sc_printf(void)
{
    // Kernel printf writes literal text and newlines itself, %i through print. Its VGA cursor
    // stays on the last cell it wrote (unchanged by K7), so only cells and position are compared.
    setup();
    char text[100];
    for (int n = 1; n <= 40; n++)
    {
        int count = kernel_printf("printf line %i of 40 with some text\n", n);
        int k = 0;
        for (const char *p = "printf line "; *p; p++)
        {
            text[k++] = *p;
        }
        const char *digits = itoa(n);
        for (const char *p = digits; *p; p++)
        {
            text[k++] = *p;
        }
        for (const char *p = " of 40 with some text\n"; *p; p++)
        {
            text[k++] = *p;
        }
        text[k] = 0;
        model_print(text, WHITE);
        verify("printf with %i and newline, 40 lines", 0);
        if (count != k - 1)
        {
            BAD("printf line %d returned %d, expected %d", n, count, k - 1);
        }
        if (problems)
        {
            printf("  (after line %d)\n", n);
            return;
        }
    }
}

static void sc_printf_wrap(void)
{
    // Literal text without newlines and the unknown-format path ("%q" prints q) wrap and scroll too.
    setup();
    for (int n = 0; n < 30; n++)
    {
        kernel_printf("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd%q%q");
        model_print("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdqq", WHITE);
        verify("printf, 30 rows of literal text and %q", 0);
        if (problems)
        {
            printf("  (after row %d)\n", n + 1);
            return;
        }
    }
    kernel_printf("%q");
    model_print("q", WHITE);
    verify("printf, 30 rows of literal text and %q", 0);
}

static void sc_putchar_bounds(void)
{
    // terminal_putchar must not touch memory for a position off the screen.
    static const int pos[][2] = { { 80, 0 }, { 0, 25 }, { 79, 25 }, { 80, 24 }, { 1000, 1000 }, { -1, 0 }, { 0, -1 }, { -80, 1 } };
    for (unsigned i = 0; i < sizeof(pos) / sizeof(pos[0]); i++)
    {
        setup();
        set_position(4, 4);
        terminal_putchar(pos[i][0], pos[i][1], '!', 0x4E);
        verify("terminal_putchar off the screen", 0);
        if (problems)
        {
            printf("  (position %d,%d)\n", pos[i][0], pos[i][1]);
            return;
        }
    }
}

static void sc_stress(void)
{
    // A long mixed run: every path, every line length, thousands of scrolls.
    setup();
    unsigned seed = 12345;
    char text[300];
    for (int op = 0; op < 4000; op++)
    {
        seed = seed * 1103515245 + 12345;
        int len = (seed >> 8) % 230;
        for (int i = 0; i < len; i++)
        {
            seed = seed * 1103515245 + 12345;
            text[i] = (seed >> 16) % 9 == 0 ? '\n' : ' ' + 1 + (seed >> 16) % 90;
            if (text[i] == '%')
            {
                text[i] = '+';
            }
        }
        text[len] = 0;
        int how = op % 4;
        if (how == 0)
        {
            print(text);
            model_print(text, WHITE);
        }
        else if (how == 1)
        {
            printc(text, 2 + op % 13);
            model_print(text, 2 + op % 13);
        }
        else if (how == 2)
        {
            kernel_printf(text);
            model_print(text, WHITE);
        }
        else
        {
            for (int i = 0; i < len; i++)
            {
                terminal_writechar(text[i], WHITE);
            }
            model_print(text, WHITE);
        }
        // Kernel printf leaves the VGA cursor on the last cell it wrote (as before), so the cursor
        // is only compared after the other paths when they printed something.
        verify("mixed output, 4000 calls", how != 2 && len > 0);
        if (problems)
        {
            printf("  (after call %d)\n", op + 1);
            return;
        }
    }
}

// --- group cursor: terminal_move_cursor (syscall 11) ---

// One move: the result, and on success the new row/column and CRT cursor; on failure nothing
// changed, not even the CRT cursor. No screen cell changes either way.
static void move(int delta, int want_result)
{
    int before_reg = cursor_reg;
    int result = terminal_move_cursor(delta);
    if (result != want_result)
    {
        BAD("move %d from (%d,%d) returned %d, expected %d", delta, want_col, want_row, result, want_result);
    }
    if (want_result == 0)
    {
        int to = want_row * COLS + want_col + delta;
        want_row = to / COLS;
        want_col = to % COLS;
        verify("cursor move", 1);
    }
    else
    {
        verify("refused cursor move", 0);
        if (cursor_reg != before_reg)
        {
            BAD("refused move %d changed the VGA cursor to %d", delta, cursor_reg);
        }
    }
}

static void cu_row(void)
{
    setup();
    set_position(10, 3);
    move(-4, 0);
    move(7, 0);
    move(0, 0);
    terminal_writechar('Q', 0x0E);
    model_put('Q', 0x0E);
    verify("character after moving within a row", 1);
}

static void cu_rows(void)
{
    setup();
    set_position(0, 5);
    move(-1, 0);    // column 79 of the row above
    move(1, 0);
    set_position(78, 6);
    move(3, 0);     // (1,7)
    move(-250, 0);
    move(333, 0);
    move(-COLS, 0);
    print("ab");
    model_print("ab", WHITE);
    verify("print after moving across rows", 1);
    terminal_backspace();
    model_backspace();
    verify("backspace after moving across rows", 1);
}

static void cu_edges(void)
{
    setup();
    set_position(0, 0);
    move(0, 0);
    move(-1, -EINVARG);
    move(CELLS - 1, 0);    // the last cell
    move(1, -EINVARG);
    move(0, 0);
    terminal_writechar('E', WHITE);   // the last cell, then the screen scrolls
    model_put('E', WHITE);
    verify("character in the last cell after a move", 1);
    move(-(CELLS - 1), -EINVARG);  // (0,24) after the scroll: only 1920 cells before it
    move(-(CELLS - COLS), 0);
}

static void cu_extremes(void)
{
    static const int bad[] = { -2147483647 - 1, 2147483647, -1001, 1000, -2000, 2000, CELLS, -CELLS,
                               0x40000000, -0x40000000 };
    setup();
    set_position(40, 12);    // cell 1000: 1000 cells before it, 999 after
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        move(bad[i], -EINVARG);
    }
    move(-1000, 0);
    move(1999, 0);
    move(-1999, 0);
}

static void cu_after_scroll(void)
{
    setup();
    char line[40];
    for (int n = 1; n <= 30; n++)
    {
        numbered(line, "scrolled ", n, 30);
        line[30] = '\n';
        line[31] = 0;
        print(line);
        model_print(line, WHITE);
    }
    print("tail");
    model_print("tail", WHITE);
    verify("30 lines before the moves", 1);
    move(-3, 0);
    move(-COLS, 0);
    terminal_writechar('M', 0x0C);
    model_put('M', 0x0C);
    verify("character after moving on a scrolled screen", 1);
    move(COLS + 1, 0);
    print("XYZ\n");
    model_print("XYZ\n", WHITE);
    verify("print and scroll after a move", 1);
}

struct test_case
{
    const char *group;
    const char *name;
    void (*run)(void);
};

static const struct test_case cases[] = {
    { "backspace", "backspace at (0,0) changes nothing", bs_origin },
    { "backspace", "backspace in the middle of a row", bs_middle },
    { "backspace", "backspace at column 0 goes to column 79 of the row above", bs_row_start },
    { "backspace", "the next character lands on the erased cell", bs_row_start_then_char },
    { "backspace", "terminal_backspace() at column 0", bs_direct },
    { "backspace", "print of 80 characters, backspace and a replacement", bs_print },
    { "backspace", "84 backspaces across two rows", bs_across_rows },
    { "backspace", "backspaces past (0,0) stop there", bs_to_origin },
    { "backspace", "backspace on the last row", bs_last_row },
    { "scroll", "the screen is 80 x 25", sc_size },
    { "scroll", "print of 64 lines scrolls", sc_print_lines },
    { "scroll", "80-character blocks wrap and scroll", sc_wrap },
    { "scroll", "printc colours move with the text", sc_colours },
    { "scroll", "terminal_writechar scrolls", sc_putchar },
    { "scroll", "printf literal text, newline and %i scroll", sc_printf },
    { "scroll", "printf literal text and unknown format wrap and scroll", sc_printf_wrap },
    { "scroll", "terminal_putchar ignores positions off the screen", sc_putchar_bounds },
    { "scroll", "4000 mixed output calls stay on the screen", sc_stress },
    { "cursor", "moves within a row, then a character lands there", cu_row },
    { "cursor", "moves across rows, then print and backspace", cu_rows },
    { "cursor", "first and last cell, one past each refused", cu_edges },
    { "cursor", "INT_MIN, INT_MAX and off-page moves change nothing", cu_extremes },
    { "cursor", "moves on a scrolled screen", cu_after_scroll },
};

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "backspace") != 0 && strcmp(argv[1], "scroll") != 0 &&
                      strcmp(argv[1], "cursor") != 0))
    {
        printf("usage: tty-screen backspace|scroll|cursor\n");
        return 2;
    }
    const char *group = argv[1];
    height = VGA_HEIGHT;
    if (height > 25)
    {
        printf("VGA_HEIGHT %d is more than the 25 rows of the screen\n", height);
        return 2;
    }

    // One page at 0xB8000, the pages next to it unmapped.
    unsigned long lo = VGA_ADDRESS - PAGE;
    void *m = mmap((void *)lo, 3 * PAGE, 3 /* PROT_READ | PROT_WRITE */,
                   0x22 | 0x100000 /* MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE */, -1, 0);
    if (m != (void *)lo)
    {
        printf("could not map the VGA page at 0x%lx\n", VGA_ADDRESS);
        return 2;
    }
    if (munmap((void *)lo, PAGE) != 0 || munmap((void *)(VGA_ADDRESS + PAGE), PAGE) != 0)
    {
        printf("could not unmap the guard pages\n");
        return 2;
    }

    int total = 0, failed = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        if (strcmp(cases[i].group, group) != 0)
        {
            continue;
        }
        total++;
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            problems = 0;
            cases[i].run();
            fflush(0);
            _exit(problems ? 1 : 0);
        }
        int status = 0;
        if (pid < 0 || waitpid(pid, &status, 0) != pid)
        {
            printf("  FAIL %s: could not run the case\n", cases[i].name);
            failed++;
            continue;
        }
        if (WIFSIGNALED(status))
        {
            printf("  FAIL %s: stopped by signal %d (a store outside the screen page)\n", cases[i].name,
                   WTERMSIG(status));
            failed++;
        }
        else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            printf("  FAIL %s\n", cases[i].name);
            failed++;
        }
        else
        {
            printf("  ok   %s\n", cases[i].name);
        }
    }
    printf("tty-screen %s host check: %d of %d cases failed\n", group, failed, total);
    return failed ? 1 : 0;
}
