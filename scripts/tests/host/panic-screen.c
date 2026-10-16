/*
 * Host check for scripts/tests/panic-screen.sh: the real PANIC (kernel/arch/i686/sys.c) with the
 * real terminal (tty.c) and string functions (string.c), built with the host C compiler and
 * AddressSanitizer. The terminal writes to a VGA text page mapped at 0xB8000 with unmapped pages
 * around it; the bytes after the 80 x 25 screen hold a pattern that must stay.
 *
 * Stubs: outb (port 0xE9 bytes are kept as the debugcon output), IRQ_OFF (records how much had
 * been output by then), PAUSE (the halt: jumps back to the case), and kmalloc/kzalloc/kfree/
 * malloc/free, which fail the case. sys.c is built with itoa and strcat renamed to functions that
 * fail the case too: the line number goes through a local buffer, not itoa's shared one, and the
 * debug line is not built by concatenation.
 *
 * The expected screen comes from a model: the copyright row of terminal_initialize, then the
 * unchanged advice lines, "Reason: <message>" on one row, "at <file>:<line>" on the next, and
 * nothing after that, all white on red, every other cell
 * as terminal_initialize left it. Control characters in the message and file show as spaces.
 * The message shows at most 71 characters and the file at most 64 (the rest is cut, optionally
 * marked with up to three of . ~ >), so each fits one row. Message and file strings end at an
 * unmapped page, so reading past their NUL stops the case.
 *
 * Usage: panic-screen [case...]   (no argument: every case)
 * Prints "PASS <case>" or "FAIL <case>" per case, then
 * "panic-screen host check: <failed> of <cases> cases failed".
 */
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <setjmp.h>
#include "include/kernel/sys.h"
#include "host.h"

void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
int munmap(void *addr, unsigned long length);
int mprotect(void *addr, unsigned long length, int prot);
int strcmp(const char *a, const char *b);
void *host_memcpy(void *d, const void *s, unsigned long n) __asm__("memcpy");
int host_snprintf(char *str, unsigned long size, const char *format, ...) __asm__("snprintf");
char *host_strstr(const char *h, const char *n) __asm__("strstr");
unsigned int alarm(unsigned int seconds);

static unsigned long host_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
    {
        n++;
    }
    return n;
}

#define VGA_ADDRESS 0xB8000UL
#define PAGE 4096UL
#define COLS 80
#define ROWS 25
#define CELLS (COLS * ROWS)
#define GUARD 0xCC
#define PANIC_ATTR 0x4F    // white on red

static volatile uint16_t *const vga = (volatile uint16_t *)VGA_ADDRESS;
static volatile uint8_t *const vga_bytes = (volatile uint8_t *)VGA_ADDRESS;

static int problems;
#define BAD(...) do { if (problems++ < 12) { printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

// --- stubs --------------------------------------------------------------------------------------

static char debugcon[65536];
static int debug_len;
static int irq_off_calls = -1, irq_off_debug = -1, irq_off_screen = -1, pause_calls;
static jmp_buf halt;
static uint16_t initial[CELLS];

void outb(unsigned short port, unsigned char val)
{
    if (port == 0xE9 && debug_len < (int)sizeof(debugcon) - 1)
    {
        debugcon[debug_len++] = val;
    }
}

unsigned char insb(unsigned short port)
{
    return 0;
}

static int screen_touched(void)
{
    for (int i = 0; i < CELLS; i++)
    {
        if (vga[i] != initial[i])
        {
            return 1;
        }
    }
    return 0;
}

void IRQ_OFF(void)
{
    if (irq_off_calls < 0)
    {
        irq_off_calls = 0;
        irq_off_debug = debug_len;
        irq_off_screen = screen_touched();
    }
    irq_off_calls++;
}

void IRQ_RES(void)
{
    BAD("PANIC turned interrupts back on (IRQ_RES)");
}

void IRQ_ON_AND_PAUSE(void)
{
    BAD("PANIC called IRQ_ON_AND_PAUSE");
}

void PAUSE(void)
{
    pause_calls++;
    longjmp(halt, 1);
}

static void forbidden(const char *what)
{
    printf("  FAIL PANIC called %s\n", what);
    fflush(0);
    _exit(5);
}

void *kmalloc(size_t size) { forbidden("kmalloc"); return 0; }
void *kzalloc(size_t size) { forbidden("kzalloc"); return 0; }
void kfree(void *ptr) { forbidden("kfree"); }
char *panic_itoa(int i) { forbidden("itoa (shared static buffer)"); return 0; }
char *panic_strcat(char *d, const char *s) { forbidden("strcat"); return 0; }

// --- guarded strings ----------------------------------------------------------------------------

// A copy of <s> (<n> bytes plus the NUL) whose NUL is the last readable byte.
static const char *edge_n(const char *s, unsigned long n)
{
    unsigned long pages = (n + 1 + PAGE - 1) / PAGE + 1;
    char *m = mmap(0, (pages + 1) * PAGE, 3, 0x22, -1, 0);
    if (m == (char *)-1)
    {
        printf("  mmap failed\n");
        _exit(3);
    }
    mprotect(m + pages * PAGE, PAGE, 0);
    char *p = m + pages * PAGE - (n + 1);
    host_memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static const char *edge(const char *s)
{
    return s ? edge_n(s, host_strlen(s)) : 0;
}

static char *repeat(char c0, unsigned long n)
{
    static char buf[8192];
    for (unsigned long i = 0; i < n; i++)
    {
        buf[i] = c0 + i % 26;
    }
    buf[n] = 0;
    return buf;
}

// --- the model ----------------------------------------------------------------------------------

static const char *const advice[] = {
    "A problem has been detected and RorthOX has been shut down to prevent damage to your computer.\n",
    "If this is the first time you've seen this stop error screen, restart your computer. If this screen appears again, follow these steps: \n",
    "Check to make sure any new hardware or software is properly installed.\n",
    "If this is a new install, check your software/hardware vendor for\n",
    "any drivers or OS updates you may need.\n",
    "If problems continue, remove any newly installed hardware or software.\n",
    "Disable BIOS memory options such as caching or shadowing.\n",
    "\n",
    "Finally, if all else fails, go to https://github.com/oriapp/RorthOX/issues\n",
    "and submit an issue with the information below.\n",
    "\n",
};

static uint16_t want[CELLS];
static int m_row, m_col, m_scrolled;

static void m_put(char c)
{
    if (c == '\n')
    {
        m_row++;
        m_col = 0;
    }
    else
    {
        if (m_row < ROWS)
        {
            want[m_row * COLS + m_col] = (uint8_t)c | PANIC_ATTR << 8;
        }
        m_col++;
        if (m_col >= COLS)
        {
            m_col = 0;
            m_row++;
        }
    }
    if (m_row >= ROWS)
    {
        m_scrolled = 1;
    }
}

static void m_print(const char *s)
{
    for (; *s; s++)
    {
        m_put(*s);
    }
}

// The screen terminal_initialize(red) leaves: NUL characters, the copyright on row 0, position (0,1).
static void m_start(void)
{
    static const char copyright[] = "Copyright (C) R C N, GPL-2.0";
    for (int i = 0; i < CELLS; i++)
    {
        want[i] = PANIC_ATTR << 8;
    }
    for (unsigned i = 0; i < sizeof(copyright); i++)
    {
        want[i] = (uint8_t)copyright[i] | PANIC_ATTR << 8;
    }
    m_row = 1;
    m_col = 0;
    m_scrolled = 0;
    for (unsigned i = 0; i < sizeof(advice) / sizeof(advice[0]); i++)
    {
        m_print(advice[i]);
    }
}

static int reason_row;   // where "Reason: " must be

// Screen text of a field: control characters as spaces.
static void shown(char *out, const char *s, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
    {
        unsigned char c = s[i];
        out[i] = c < 32 ? ' ' : c;
    }
    out[n] = 0;
}

static void row_text(int row, char *out)
{
    for (int i = 0; i < COLS; i++)
    {
        unsigned char c = vga[row * COLS + i] & 0xFF;
        out[i] = (c >= 32 && c < 127) ? c : (c == 0 ? ' ' : '?');
    }
    out[COLS] = 0;
    for (int i = COLS - 1; i >= 0 && out[i] == ' '; i--)
    {
        out[i] = 0;
    }
}

// --- running PANIC ------------------------------------------------------------------------------

static void run_panic(const char *msg, const char *file, int line)
{
    // Every cell and the bytes after the screen set to known values first.
    for (int i = 0; i < CELLS; i++)
    {
        vga[i] = initial[i] = (uint16_t)('a' + i % 26) | (uint16_t)(0x17 << 8);
    }
    for (unsigned long i = CELLS * 2; i < PAGE; i++)
    {
        vga_bytes[i] = GUARD;
    }
    debug_len = 0;
    irq_off_calls = -1;
    pause_calls = 0;
    // Through a pointer without noreturn, so a PANIC that returns is seen.
    void (*volatile panic)(const char *, const char *, int) = (void (*)(const char *, const char *, int))PANIC;
    if (setjmp(halt) == 0)
    {
        panic(msg, file, line);
        BAD("PANIC returned");
        return;
    }
    if (pause_calls != 1)
    {
        BAD("PAUSE called %d times", pause_calls);
    }
    if (irq_off_calls < 0)
    {
        BAD("IRQ_OFF never called");
    }
    else if (irq_off_debug != 0 || irq_off_screen)
    {
        BAD("IRQ_OFF called after output (%d debugcon bytes, screen %s)", irq_off_debug,
            irq_off_screen ? "changed" : "untouched");
    }
    for (unsigned long i = CELLS * 2; i < PAGE; i++)
    {
        if (vga_bytes[i] != GUARD)
        {
            BAD("byte 0x%lx after the screen was written", VGA_ADDRESS + i);
            break;
        }
    }
    char row0[COLS + 1];
    row_text(0, row0);
    if (strcmp(row0, "Copyright (C) R C N, GPL-2.0") != 0)
    {
        BAD("row 0 is \"%s\", not the copyright line", row0);
    }
}

// Compare every cell with the model.
static void expect_screen(const char *what)
{
    if (m_scrolled)
    {
        BAD("%s: the model does not fit 25 rows", what);
    }
    for (int r = 0; r < ROWS; r++)
    {
        for (int c = 0; c < COLS; c++)
        {
            int i = r * COLS + c;
            if (vga[i] != want[i])
            {
                char got[COLS + 1];
                row_text(r, got);
                BAD("%s: cell (%d,%d) is 0x%02x/0x%02x, expected 0x%02x/0x%02x; row %d is \"%s\"", what, c, r,
                    vga[i] & 0xFF, vga[i] >> 8, want[i] & 0xFF, want[i] >> 8, r, got);
                break;
            }
        }
    }
}

// The whole screen for a message and file that need no cutting.
static void exact_case(const char *what, const char *msg, const char *file, int line)
{
    char tmp[4096];
    run_panic(edge(msg), edge(file), line);
    m_start();
    reason_row = m_row;
    shown(tmp, msg, host_strlen(msg));
    m_print("Reason: ");
    m_print(tmp);
    m_print("\n");
    m_print("at ");
    shown(tmp, file, host_strlen(file));
    m_print(tmp);
    host_snprintf(tmp, sizeof(tmp), ":%d\n", line);
    m_print(tmp);
    expect_screen(what);
}

static int debug_has(const char *s)
{
    debugcon[debug_len] = 0;
    return host_strstr(debugcon, s) != 0;
}

// --- cases --------------------------------------------------------------------------------------

static void c_basic(void)
{
    exact_case("basic", "Kernel exception", "./kernel/idt/idt.c", 123);
    if (!debug_has("\033[1m[PANIC]: Kernel exception at ./kernel/idt/idt.c:123\033[0m"))
    {
        BAD("debugcon has no \"[PANIC]: Kernel exception at ./kernel/idt/idt.c:123\" line: \"%s\"", debugcon);
    }
    if (reason_row != 14)
    {
        BAD("the model puts the reason on row %d, not 14", reason_row);
    }
}

static void c_newline(void)
{
    // The real boot panic: its message ends with a newline, which shows as a space.
    exact_case("shell message", "Failed to load shell.elf\n", "./kernel/kernel.c", 34);
    if (!debug_has("[PANIC]: Failed to load shell.elf"))
    {
        BAD("debugcon has no \"[PANIC]: Failed to load shell.elf\"");
    }
}

static void c_empty(void)
{
    exact_case("empty strings", "", "", 0);
    if (!debug_has("[PANIC]:  at :0"))
    {
        BAD("debugcon has no \"[PANIC]:  at :0\"");
    }
}

static void c_format(void)
{
    exact_case("format characters", "%x %s %% %n %d", "%s.c", 7);
    if (!debug_has("[PANIC]: %x %s %% %n %d at %s.c:7"))
    {
        BAD("debugcon does not show the message and file as they are");
    }
}

static void c_control(void)
{
    exact_case("control characters", "a\nb\rc\bd\te\001f\033g", "x\ny\bz.c", 5);
}

static void c_lines(void)
{
    exact_case("INT_MAX line", "m", "f.c", INT_MAX);
    if (!debug_has("[PANIC]: m at f.c:2147483647"))
    {
        BAD("debugcon has no line 2147483647");
    }
    exact_case("INT_MIN line", "m", "f.c", INT_MIN);
    if (!debug_has("[PANIC]: m at f.c:-2147483648"))
    {
        BAD("debugcon has no line -2147483648");
    }
    exact_case("negative line", "m", "f.c", -1);
}

// Exactly the longest message (71) and file (64, with an 11-character line) that fit.
static void c_fit(void)
{
    char msg[80], file[80];
    host_memcpy(msg, repeat('A', 71), 72);
    host_memcpy(file, repeat('a', 64), 65);
    exact_case("71-character message, 64-character file", msg, file, INT_MIN);
}

static int cut_ok(const char *field, const char *full, unsigned long max, unsigned long min_kept)
{
    unsigned long n = host_strlen(field), k = 0;
    if (n > max)
    {
        return 0;
    }
    while (k < n && field[k] == full[k])
    {
        k++;
    }
    for (unsigned long i = k; i < n; i++)
    {
        if (field[i] != '.' && field[i] != '~' && field[i] != '>')
        {
            return 0;
        }
    }
    return n - k <= 3 && k >= min_kept;
}

// Longer than one row: cut, and everything after still in place.
static void c_long(void)
{
    static const unsigned long lengths[][2] = { { 72, 65 }, { 79, 70 }, { 200, 150 }, { 3000, 3000 } };
    for (unsigned t = 0; t < 4; t++)
    {
        char msg[4096], file[4096], row[COLS + 1], what[64];
        host_memcpy(msg, repeat('A', lengths[t][0]), lengths[t][0] + 1);
        host_memcpy(file, repeat('a', lengths[t][1]), lengths[t][1] + 1);
        host_snprintf(what, sizeof(what), "%lu-character message, %lu-character file", lengths[t][0], lengths[t][1]);
        run_panic(edge(msg), edge(file), INT_MIN);
        m_start();
        int r = m_row;
        row_text(r, row);
        if (host_strstr(row, "Reason: ") != row || !cut_ok(row + 8, msg, 71, 64))
        {
            BAD("%s: reason row is \"%s\"", what, row);
        }
        row_text(r + 1, row);
        const char *colon = row + host_strlen(row) - 12;
        if (host_strstr(row, "at ") != row || host_strlen(row) < 15 || strcmp(colon, ":-2147483648") != 0)
        {
            BAD("%s: location row is \"%s\"", what, row);
        }
        else
        {
            char f[COLS + 1];
            host_memcpy(f, row + 3, colon - (row + 3));
            f[colon - (row + 3)] = 0;
            if (!cut_ok(f, file, 64, 56))
            {
                BAD("%s: file shown as \"%s\"", what, f);
            }
        }
        for (int i = r + 2; i < ROWS; i++)
        {
            row_text(i, row);
            if (row[0])
            {
                BAD("%s: row %d is \"%s\", expected empty", what, i, row);
            }
        }
        // debugcon: the start of both fields, and a bounded line.
        char head[1100];
        host_snprintf(head, sizeof(head), "[PANIC]: %.*s", (int)(lengths[t][0] < 1000 ? lengths[t][0] : 1000), msg);
        if (!debug_has(head))
        {
            BAD("%s: debugcon does not start with the message (%d bytes: \"%.60s\")", what, debug_len, debugcon);
        }
        host_snprintf(head, sizeof(head), "%.*s", (int)(lengths[t][1] < 1000 ? lengths[t][1] : 1000), file);
        if (!debug_has(head))
        {
            BAD("%s: debugcon does not have the file", what);
        }
        if (debug_len > 2 * 1023 + 200)
        {
            BAD("%s: %d debugcon bytes, more than two 1023-character fields", what, debug_len);
        }
    }
}

// NULL message or file: some fixed text instead, the rest of the screen in place.
static void c_null(void)
{
    static const char *const msgs[] = { 0, "msg", 0 };
    static const char *const files[] = { "file.c", 0, 0 };
    for (int t = 0; t < 3; t++)
    {
        char row[COLS + 1];
        run_panic(edge(msgs[t]), edge(files[t]), 9);
        m_start();
        int r = m_row;
        row_text(r, row);
        if (host_strstr(row, "Reason: ") != row || host_strlen(row) > 79 || (msgs[t] && strcmp(row, "Reason: msg") != 0))
        {
            BAD("NULL case %d: reason row is \"%s\"", t, row);
        }
        row_text(r + 1, row);
        unsigned long n = host_strlen(row);
        if (host_strstr(row, "at ") != row || n < 5 || strcmp(row + n - 2, ":9") != 0 ||
            (files[t] && strcmp(row, "at file.c:9") != 0))
        {
            BAD("NULL case %d: location row is \"%s\"", t, row);
        }
        row_text(r + 2, row);
        if (row[0])
        {
            BAD("NULL case %d: row %d is \"%s\", expected empty", t, r + 2, row);
        }
    }
}

// PANIC from the old debug-buffer bound: a message just under 1000 characters and a long file.
static void c_debug_overflow(void)
{
    char msg[1000], file[1100];
    host_memcpy(msg, repeat('A', 999), 1000);
    host_memcpy(file, repeat('a', 1000), 1001);
    run_panic(edge(msg), edge(file), 1);
    char head[1100];
    host_snprintf(head, sizeof(head), "[PANIC]: %.999s at ", msg);
    if (!debug_has(head))
    {
        BAD("debugcon does not have the 999-character message followed by \" at \"");
    }
}

struct test_case
{
    const char *name;
    void (*run)(void);
};

static const struct test_case cases[] = {
    { "basic", c_basic },
    { "newline", c_newline },
    { "empty", c_empty },
    { "format", c_format },
    { "control", c_control },
    { "lines", c_lines },
    { "fit", c_fit },
    { "long", c_long },
    { "null", c_null },
    { "debug_overflow", c_debug_overflow },
};

int main(int argc, char **argv)
{
    unsigned long lo = VGA_ADDRESS - PAGE;
    void *m = mmap((void *)lo, 3 * PAGE, 3, 0x22 | 0x100000 /* MAP_FIXED_NOREPLACE */, -1, 0);
    if (m != (void *)lo || munmap((void *)lo, PAGE) != 0 || munmap((void *)(VGA_ADDRESS + PAGE), PAGE) != 0)
    {
        printf("could not map the VGA page at 0x%lx\n", VGA_ADDRESS);
        return 2;
    }
    int total = 0, failed = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int wanted = argc < 2;
        for (int a = 1; a < argc; a++)
        {
            wanted |= strcmp(argv[a], cases[i].name) == 0;
        }
        if (!wanted)
        {
            continue;
        }
        total++;
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(20);
            problems = 0;
            cases[i].run();
            fflush(0);
            _exit(problems ? 1 : 0);
        }
        int status = 0;
        if (pid < 0 || waitpid(pid, &status, 0) != pid)
        {
            printf("FAIL %s: could not run the case\n", cases[i].name);
            failed++;
            continue;
        }
        if (WIFSIGNALED(status))
        {
            printf("FAIL %s: stopped by signal %d\n", cases[i].name, WTERMSIG(status));
            failed++;
        }
        else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            printf("FAIL %s (exit %d)\n", cases[i].name, WEXITSTATUS(status));
            failed++;
        }
        else
        {
            printf("PASS %s\n", cases[i].name);
        }
    }
    printf("panic-screen host check: %d of %d cases failed\n", failed, total);
    return failed ? 1 : 0;
}
