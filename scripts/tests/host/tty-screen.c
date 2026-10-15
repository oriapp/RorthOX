/*
 * Host check for scripts/tests/tty-backspace.sh: the real
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
 * Usage: tty-screen backspace
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

static void model_newline(void)
{
    want_col = 0;
    want_row++;
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
};

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "backspace") != 0)
    {
        printf("usage: tty-screen backspace\n");
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
