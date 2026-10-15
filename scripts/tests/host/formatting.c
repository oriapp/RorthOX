/*
 * Host check for scripts/tests/formatting.sh: the real kernel/string/string.c and
 * kernel/arch/i686/tty.c, built with the host C compiler.
 *
 * string.c is built with its formatting names prefixed (kernel_snprintf, kernel_sprintf,
 * kernel_vsnprintf, kernel_vsprintf, kernel_itoa_hex), so no call can end up in the C library's
 * formatter. The expected text comes from the C library's snprintf, reached by its symbol name.
 * tty.c writes to a VGA text page mapped at its real address 0xB8000 with unmapped pages around it.
 *
 * Every destination ends right before an unmapped page, and the bytes before it in its page hold a
 * pattern that must stay, so a store past the given size stops the case with SIGSEGV. Format
 * strings and %s arguments are copied to the end of a page too: their NUL is the last readable
 * byte, so reading past it also stops the case. Every case runs in its own process with a time
 * limit (a formatter that calls itself forever overflows the stack or runs out of time).
 *
 * Buffer formatter (vsnprintf and the wrappers around it): %d %i %u %x %X %c %s %% and %02d.
 * Return value: the length of the whole output (without the NUL), whatever the size. Size 0 writes
 * nothing (the destination may be NULL); otherwise at most size - 1 characters and always a NUL.
 * A NULL destination with a size above 0 returns -1 and writes nothing.
 * An unsupported conversion returns -1 with a NUL-terminated prefix in the buffer. A lone % at the
 * end of the format is printed as %. An output longer than INT_MAX returns -1.
 * itoa_hex: digits in the right order (uppercase), the sign in front, INT_MIN, bases 2-36 only
 * (any other base gives an empty string).
 * Terminal: print_hex shows 0x and 8 uppercase digits; printf("%x") shows the same and counts 10;
 * a % at the end of the format is shown as % and the format is not read past its NUL; %i, %s,
 * %b, %B, unknown letters and newlines keep their output and count.
 *
 * Usage: formatting [case...]   (no argument: every case)
 * Prints "PASS <case>" or "FAIL <case>" per case, then
 * "formatting host check: <failed> of <cases> cases failed".
 */
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

// tty.c has its own printf and vprintf: rename them so the check can use the host printf.
#define printf kernel_printf
#define vprintf kernel_vprintf
#include "arch/i686/tty.c"
#undef printf
#undef vprintf

#include "host.h"

// The kernel's buffer formatter (string.c is built with -Dvsnprintf=kernel_vsnprintf and so on,
// and this file is built with the same names). Declared here as well in case string.h doesn't.
int vsnprintf(char *str, size_t size, const char *format, va_list args);
int vsprintf(char *str, const char *format, va_list args);

// The C library's snprintf, for the expected text.
int host_snprintf(char *str, unsigned long size, const char *format, ...) __asm__("snprintf");
void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
int munmap(void *addr, unsigned long length);
int mprotect(void *addr, unsigned long length, int prot);
int strcmp(const char *a, const char *b);
void *host_memcpy(void *d, const void *s, unsigned long n) __asm__("memcpy");
void *host_memset(void *d, int c, unsigned long n) __asm__("memset");
unsigned int alarm(unsigned int seconds);

#define VGA_ADDRESS 0xB8000UL
#define PAGE 4096UL
#define COLS 80
#define CELLS (COLS * 25)
#define GUARD 0xA5

static volatile uint16_t *const vga = (volatile uint16_t *)VGA_ADDRESS;

void outb(unsigned short port, unsigned char val)
{
}

unsigned char insb(unsigned short port)
{
    return 0;
}

static int problems;

#define BAD(...) do { if (problems++ < 12) { printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

// --- guarded memory -----------------------------------------------------------------------------

// A writable area of <bytes> that ends right before an unmapped page.
static char *page_end(unsigned long bytes)
{
    unsigned long pages = (bytes + PAGE - 1) / PAGE + 1;
    char *m = mmap(0, (pages + 1) * PAGE, 3, 0x22, -1, 0);
    if (m == (char *)-1)
    {
        printf("  mmap failed\n");
        _exit(3);
    }
    mprotect(m + pages * PAGE, PAGE, 0);
    return m + pages * PAGE - bytes;
}

// A copy of <s> whose NUL is the last readable byte.
static const char *edge(const char *s)
{
    unsigned long n = strlen(s) + 1;
    char *p = page_end(n);
    host_memcpy(p, s, n);
    return p;
}

// A destination of <size> bytes before an unmapped page, the rest of its page set to GUARD.
struct dest
{
    char *base;   // start of the page area that is checked
    char *buf;
    unsigned long size;
};

static struct dest dest_new(unsigned long size)
{
    struct dest d;
    d.buf = page_end(size);
    d.size = size;
    d.base = (char *)((unsigned long)d.buf & ~(PAGE - 1));
    if (size > 0 && d.base == d.buf + size)
    {
        d.base -= PAGE;
    }
    host_memset(d.base, GUARD, d.buf + size - d.base);
    return d;
}

// The bytes before the destination must still hold the pattern.
static int dest_guard_ok(struct dest *d)
{
    for (char *p = d->base; p < d->buf; p++)
    {
        if ((unsigned char)*p != GUARD)
        {
            return 0;
        }
    }
    return 1;
}

static void show(char *out, const char *s, int n)
{
    int j = 0;
    for (int i = 0; i < n && j < 60; i++)
    {
        unsigned char c = s[i];
        if (c >= 32 && c < 127)
        {
            out[j++] = c;
        }
        else
        {
            j += host_snprintf(out + j, 8, "\\x%02x", c);
        }
    }
    out[j] = 0;
}

// Check one buffer-formatter result: <ret> from a call with <size>, against the full output
// <want> of length <n> (may contain NUL bytes).
static void check_result(const char *what, struct dest *d, int ret, const char *want, int n)
{
    char a[256], b[256];
    if (ret != n)
    {
        BAD("%s, size %lu: returned %d, expected %d", what, d->size, ret, n);
    }
    if (!dest_guard_ok(d))
    {
        BAD("%s, size %lu: a byte before the destination changed", what, d->size);
    }
    if (d->size == 0)
    {
        return;
    }
    unsigned long keep = (unsigned long)n < d->size - 1 ? (unsigned long)n : d->size - 1;
    for (unsigned long i = 0; i < keep; i++)
    {
        if (d->buf[i] != want[i])
        {
            show(a, d->buf, keep);
            show(b, want, keep);
            BAD("%s, size %lu: buffer \"%s\", expected \"%s\"", what, d->size, a, b);
            return;
        }
    }
    if (d->buf[keep] != 0)
    {
        BAD("%s, size %lu: no NUL at index %lu (0x%02x)", what, d->size, keep, (unsigned char)d->buf[keep]);
    }
    for (unsigned long i = keep + 1; i < d->size; i++)
    {
        if ((unsigned char)d->buf[i] != GUARD)
        {
            BAD("%s, size %lu: byte %lu after the NUL changed", what, d->size, i);
            return;
        }
    }
}

// The kernel's vsnprintf and vsprintf through a variadic front, to test the va_list entry points.
static int call_vsnprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(str, size, format, ap);
    va_end(ap);
    return r;
}

static int call_vsprintf(char *str, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = vsprintf(str, format, ap);
    va_end(ap);
    return r;
}

// Format with every size from 0 to the full length + 2, each into a fresh guarded destination.
// The format and the string argument are copies at the end of a page.
#define EVERY_SIZE(what, want, n, fmt, ...)                                                   \
    do {                                                                                      \
        const char *f_ = edge(fmt);                                                           \
        for (unsigned long size_ = 0; size_ <= (unsigned long)(n) + 2; size_++)               \
        {                                                                                     \
            struct dest d_ = dest_new(size_);                                                 \
            int r_ = snprintf(size_ ? d_.buf : 0, size_, f_, __VA_ARGS__);                     \
            check_result(what, &d_, r_, want, n);                                             \
        }                                                                                     \
    } while (0)

// --- buffer formatter cases ---------------------------------------------------------------------

static void c_api(void)
{
    const char *fmt = edge("a%db%sc");
    const char *s = edge("xy");
    struct dest d;

    d = dest_new(16);
    check_result("snprintf", &d, snprintf(d.buf, 16, fmt, -42, s), "a-42bxyc", 8);
    d = dest_new(9);
    check_result("sprintf", &d, sprintf(d.buf, fmt, -42, s), "a-42bxyc", 8);
    d = dest_new(5);
    check_result("vsnprintf (truncating)", &d, call_vsnprintf(d.buf, 5, fmt, -42, s), "a-42bxyc", 8);
    d = dest_new(9);
    check_result("vsprintf", &d, call_vsprintf(d.buf, fmt, -42, s), "a-42bxyc", 8);
    d = dest_new(0);
    check_result("vsnprintf size 0, NULL", &d, call_vsnprintf(0, 0, fmt, -42, s), "a-42bxyc", 8);
}

static void c_empty(void)
{
    struct dest d;
    const char *empty = edge("");
    const char *abc = edge("abc");
    d = dest_new(1);
    check_result("empty format, size 1", &d, snprintf(d.buf, 1, empty, 0), "", 0);
    d = dest_new(4);
    check_result("empty format, size 4", &d, snprintf(d.buf, 4, empty, 0), "", 0);
    d = dest_new(0);
    check_result("empty format, size 0, NULL", &d, snprintf(0, 0, empty, 0), "", 0);
    d = dest_new(0);
    check_result("\"abc\", size 0, NULL", &d, snprintf(0, 0, abc, 0), "abc", 3);
    d = dest_new(1);
    check_result("\"abc\", size 1", &d, snprintf(d.buf, 1, abc, 0), "abc", 3);
    d = dest_new(1);
    check_result("empty %s, size 1", &d, snprintf(d.buf, 1, edge("%s"), edge("")), "", 0);
    d = dest_new(1);
    check_result("sprintf empty", &d, sprintf(d.buf, empty), "", 0);
    // A NULL destination with a size: -1, nothing written (a store through NULL stops the case).
    int r = snprintf(0, 4, abc, 0);
    if (r != -1)
    {
        BAD("snprintf(NULL, 4, \"abc\") returned %d, expected -1", r);
    }
    r = snprintf(0, 1, empty, 0);
    if (r != -1)
    {
        BAD("snprintf(NULL, 1, \"\") returned %d, expected -1", r);
    }
    r = call_vsnprintf(0, 8, edge("a%db"), 5);
    if (r != -1)
    {
        BAD("vsnprintf(NULL, 8, \"a%%db\") returned %d, expected -1", r);
    }
    r = sprintf(0, abc);
    if (r != -1)
    {
        BAD("sprintf(NULL, \"abc\") returned %d, expected -1", r);
    }
}

static void c_truncate(void)
{
    const char *fmt = "x=%d y=%u h=%x H=%X c=%c s=%s p=%% z=%02d i=%i!";
    char want[256];
    const char *s = edge("str");
    int n = host_snprintf(want, sizeof(want), fmt, -1234, 4000000000u, 0xbeefu, 0xbeefu, 'Q', "str", 7, 99);
    EVERY_SIZE("mixed format", want, n, fmt, -1234, 4000000000u, 0xbeefu, 0xbeefu, 'Q', s, 7, 99);
    // Long output, all of it cut at every size.
    const char *fmt2 = "%s|%s|%d";
    const char *a = edge("0123456789abcdefghijklmnopqrstuvwxyz");
    const char *b = edge("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    n = host_snprintf(want, sizeof(want), fmt2, a, b, INT_MIN);
    EVERY_SIZE("two strings and INT_MIN", want, n, fmt2, a, b, INT_MIN);
}

static void c_pad(void)
{
    static const int values[] = { 0, 7, 9, 10, 12, 59, 99, 100, 123, -1, -5, -12, -123, INT_MAX, INT_MIN };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        char want[64], what[64];
        int n = host_snprintf(want, sizeof(want), "[%02d]", values[i]);
        host_snprintf(what, sizeof(what), "%%02d of %d", values[i]);
        EVERY_SIZE(what, want, n, "[%02d]", values[i]);
    }
    // The RTC callers' format.
    char want[64];
    int n = host_snprintf(want, sizeof(want), "%02d:%02d:%02d %02d/%02d/%d", 9, 5, 0, 1, 12, 2023);
    EVERY_SIZE("time format", want, n, "%02d:%02d:%02d %02d/%02d/%d", 9, 5, 0, 1, 12, 2023);
}

static void c_limits(void)
{
    static const struct { const char *fmt; unsigned v; } cases[] = {
        { "%d", 0 }, { "%d", INT_MAX }, { "%d", (unsigned)INT_MIN }, { "%d", (unsigned)-1 },
        { "%i", (unsigned)INT_MIN }, { "%i", 1000000 },
        { "%u", 0 }, { "%u", UINT_MAX }, { "%u", 0x80000000u }, { "%u", 4294967294u },
        { "%x", 0 }, { "%x", 0x80000000u }, { "%x", UINT_MAX }, { "%x", 0x1234abcdu }, { "%x", 0xf },
        { "%X", 0 }, { "%X", 0x80000000u }, { "%X", UINT_MAX }, { "%X", 0xdeadbeefu }, { "%X", 0x10 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char want[64], what[64];
        int n = host_snprintf(want, sizeof(want), cases[i].fmt, cases[i].v);
        host_snprintf(what, sizeof(what), "%s of 0x%x", cases[i].fmt, cases[i].v);
        EVERY_SIZE(what, want, n, cases[i].fmt, cases[i].v);
    }
}

static void c_char(void)
{
    // A zero byte from %c is output like any other character: counted, and copied.
    const char want[] = { 'a', 0, 'b', 'Z', '%', 0 };
    EVERY_SIZE("%c with a zero byte", want, 5, "a%cb%c%%", 0, 'Z');
    const char want2[] = { 0 };
    EVERY_SIZE("a lone %c of 0", want2, 1, "%c", 0);
}

static void c_string(void)
{
    char want[64];
    const char *s = edge("edge of a page");
    int n = host_snprintf(want, sizeof(want), "<%s>", "edge of a page");
    EVERY_SIZE("%s ending at a page end", want, n, "<%s>", s);
    // NULL: either "(null)" or nothing, with the matching count.
    struct dest d = dest_new(32);
    int r = snprintf(d.buf, 32, edge("[%s]"), (char *)0);
    if (r == 8)
    {
        check_result("%s of NULL", &d, r, "[(null)]", 8);
    }
    else
    {
        check_result("%s of NULL", &d, r, "[]", 2);
    }
}

// Unsupported conversions: -1, a NUL-terminated prefix of the text before them, and nothing read
// or written out of bounds.
static void c_unsupported(void)
{
    static const char *const fmts[] = {
        "ab%q", "ab%5d", "ab%ld", "ab%-d", "ab%.2s", "ab%p", "ab%f", "ab%n", "ab%lld", "ab%hd", "ab%03d",
        "ab%2d", "ab%0x", "ab%02x", "ab%02u", "ab%02s", "ab%#x", "ab%+d", "ab% d", "ab%*d",
    };
    for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
    {
        for (unsigned long size = 0; size <= 8; size++)
        {
            struct dest d = dest_new(size);
            int pin = 0x5a5a;
            int r = snprintf(size ? d.buf : 0, size, edge(fmts[i]), 42, &pin, 42);
            if (r != -1)
            {
                BAD("\"%s\", size %lu: returned %d, expected -1", fmts[i], size, r);
            }
            if (!dest_guard_ok(&d))
            {
                BAD("\"%s\", size %lu: a byte before the destination changed", fmts[i], size);
            }
            if (pin != 0x5a5a)
            {
                BAD("\"%s\": wrote through an argument", fmts[i]);
            }
            if (size == 0)
            {
                continue;
            }
            unsigned long k;
            for (k = 0; k < size && d.buf[k] != 0; k++)
            {
            }
            if (k == size)
            {
                BAD("\"%s\", size %lu: no NUL in the buffer", fmts[i], size);
            }
            else if (k > 2 || (k > 0 && d.buf[0] != 'a') || (k > 1 && d.buf[1] != 'b'))
            {
                BAD("\"%s\", size %lu: buffer is not a prefix of \"ab\"", fmts[i], size);
            }
        }
    }
}

// A % at the very end of the format is output as %, and nothing after the NUL is read.
static void c_trailing(void)
{
    EVERY_SIZE("trailing %", "abc%", 4, "abc%", 0);
    EVERY_SIZE("lone %", "%", 1, "%", 0);
    EVERY_SIZE("%d then trailing %", "7%", 2, "%d%", 7);
    EVERY_SIZE("%% then trailing %", "%%", 2, "%%%", 0);
    // Incomplete %02d at the end: no read past the NUL (the result is not fixed here).
    static const char *const cut[] = { "x%0", "x%02", "%0", "%02" };
    for (unsigned i = 0; i < 4; i++)
    {
        struct dest d = dest_new(16);
        int r = snprintf(d.buf, 16, edge(cut[i]), 5);
        if (r != -1 && (r < 0 || r > 15 || d.buf[r] != 0))
        {
            BAD("\"%s\": returned %d without that many characters in the buffer", cut[i], r);
        }
    }
}

// Outputs up to and past INT_MAX characters (size 0, so nothing is stored).
static void c_int_max(void)
{
    unsigned long big = 64UL << 20;
    char *s = page_end(big + 1);
    host_memset(s, 'a', big);
    s[big] = 0;
    // 32 strings of 64 MiB minus one character: exactly INT_MAX.
    const char *fmt = edge("%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s");
    int r = snprintf(0, 0, fmt, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s,
                     s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s + 1);
    if (r != INT_MAX)
    {
        BAD("output of exactly INT_MAX characters returned %d", r);
    }
    // One more character: the count doesn't fit an int.
    r = snprintf(0, 0, fmt, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s,
                 s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s);
    if (r != -1)
    {
        BAD("output of INT_MAX + 1 characters returned %d, expected -1", r);
    }
    // And with a buffer: stays terminated, nothing past it written.
    struct dest d = dest_new(8);
    r = snprintf(d.buf, 8, fmt, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s,
                 s, s, s, s, s, s, s, s, s, s, s, s, s, s, s, s);
    if (r != -1)
    {
        BAD("INT_MAX + 1 characters into 8 bytes returned %d, expected -1", r);
    }
    if (d.buf[7] != 0 || !dest_guard_ok(&d))
    {
        BAD("INT_MAX + 1 characters into 8 bytes: buffer not terminated or guard changed");
    }
}

// --- itoa_hex -----------------------------------------------------------------------------------

static void hex_case(int value, int base, const char *want)
{
    unsigned long n = strlen(want) + 1;
    // Room for exactly the result (at least one byte), then an unmapped page.
    struct dest d = dest_new(n);
    char *r = itoa_hex(value, d.buf, base);
    if (r != d.buf)
    {
        BAD("itoa_hex(%d, base %d) returned %p, not the buffer", value, base, (void *)r);
    }
    if (strncmp(d.buf, want, n) != 0)
    {
        char a[256];
        show(a, d.buf, n);
        BAD("itoa_hex(%d, base %d) gave \"%s\", expected \"%s\"", value, base, a, want);
    }
    if (!dest_guard_ok(&d))
    {
        BAD("itoa_hex(%d, base %d) wrote before the buffer", value, base);
    }
}

static void c_itoa_hex(void)
{
    hex_case(0, 16, "0");
    hex_case(7, 16, "7");
    hex_case(255, 16, "FF");
    hex_case(0x1234ABCD, 16, "1234ABCD");
    hex_case(0x10, 16, "10");
    hex_case(INT_MAX, 16, "7FFFFFFF");
    hex_case(-255, 16, "-FF");
    hex_case(-1, 16, "-1");
    hex_case(-0x1234ABCD, 16, "-1234ABCD");
    hex_case(INT_MIN, 16, "-80000000");
    hex_case(10, 2, "1010");
    hex_case(-6, 2, "-110");
    hex_case(123456789, 10, "123456789");
    hex_case(INT_MIN, 10, "-2147483648");
    hex_case(35, 36, "Z");
    hex_case(36 * 36 + 35, 36, "10Z");
    hex_case(INT_MIN, 2, "-10000000000000000000000000000000");
}

static void c_itoa_base(void)
{
    static const int bases[] = { 0, 1, -16, 37, 100 };
    for (unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); i++)
    {
        hex_case(1234, bases[i], "");
        hex_case(-1234, bases[i], "");
    }
}

// --- terminal -----------------------------------------------------------------------------------

static void term_setup(void)
{
    terminal_initialize(VGA_COLOR_BLACK);
    for (int i = 0; i < CELLS; i++)
    {
        vga[i] = '.' | (0x07 << 8);
    }
    terminal_row = 3;
    terminal_col = 0;
}

static void row_text(int row, char *out)
{
    for (int i = 0; i < COLS; i++)
    {
        unsigned char c = vga[row * COLS + i] & 0xFF;
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[COLS] = 0;
    for (int i = COLS - 1; i >= 0 && (out[i] == '.' || out[i] == ' '); i--)
    {
        out[i] = 0;
    }
}

static void expect_row(const char *what, int row, const char *want)
{
    char got[COLS + 1];
    row_text(row, got);
    if (strcmp(got, want) != 0)
    {
        BAD("%s: row %d is \"%s\", expected \"%s\"", what, row, got, want);
    }
}

static void c_print_hex(void)
{
    static const struct { unsigned v; const char *want; } cases[] = {
        { 0, "0x00000000" }, { 0x1234abcd, "0x1234ABCD" }, { 0xffffffff, "0xFFFFFFFF" },
        { 0x80000001, "0x80000001" }, { 0xf, "0x0000000F" }, { 0xa0b0c0d0, "0xA0B0C0D0" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char what[64];
        host_snprintf(what, sizeof(what), "print_hex(0x%x)", cases[i].v);
        term_setup();
        print_hex(cases[i].v);
        expect_row(what, 3, cases[i].want);
        if (terminal_col != 10 || terminal_row != 3)
        {
            BAD("%s: position (%d,%d), expected (10,3)", what, terminal_col, terminal_row);
        }
    }
}

static void c_printf_hex(void)
{
    static const unsigned values[] = { 0, 0x1234abcd, 0xffffffff, 0x7, 0x80000000 };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        char what[64], want[64];
        host_snprintf(what, sizeof(what), "printf(\"[%%x]\", 0x%x)", values[i]);
        host_snprintf(want, sizeof(want), "[0x%08X]", values[i]);
        term_setup();
        int r = kernel_printf(edge("[%x]"), values[i]);
        expect_row(what, 3, want);
        if (r != 12)
        {
            BAD("%s returned %d, expected 12", what, r);
        }
    }
    term_setup();
    int r = kernel_printf(edge("%x%x"), 1u, 0xabcu);
    expect_row("printf(\"%x%x\")", 3, "0x000000010x00000ABC");
    if (r != 20)
    {
        BAD("printf(\"%%x%%x\") returned %d, expected 20", r);
    }
}

static void c_printf_trailing(void)
{
    term_setup();
    int r = kernel_printf(edge("ab%"));
    expect_row("printf(\"ab%\")", 3, "ab%");
    if (r != 3)
    {
        BAD("printf(\"ab%%\") returned %d, expected 3", r);
    }
    if (terminal_col != 3 || terminal_row != 3)
    {
        BAD("printf(\"ab%%\"): position (%d,%d), expected (3,3)", terminal_col, terminal_row);
    }
    term_setup();
    r = kernel_printf(edge("%"));
    expect_row("printf(\"%\")", 3, "%");
    if (r != 1)
    {
        BAD("printf(\"%%\") returned %d, expected 1", r);
    }
    term_setup();
    r = kernel_printf(edge("%i%"), 42);
    expect_row("printf(\"%i%\")", 3, "42%");
    if (r != 3)
    {
        BAD("printf(\"%%i%%\") returned %d, expected 3", r);
    }
}

// The other terminal conversions keep their output and their count.
static void c_printf_others(void)
{
    term_setup();
    const char *s = edge("str");
    int r = kernel_printf(edge("%i|%s|%b|%b|%q|%B"), -42, s, 1, 0, 2u);
    // %i: -42 (3), %s: 3, %b counts "true"/"false" (4 + 5) but shows True/False, %q: q (1),
    // %B counts 32 and shows 0b + 32 digits, plus five '|'.
    expect_row("printf of %i %s %b %q %B", 3, "-42|str|True|False|q|0b00000000000000000000000000000010");
    if (r != 3 + 3 + 4 + 5 + 1 + 32 + 5)
    {
        BAD("printf of %%i %%s %%b %%q %%B returned %d, expected %d", r, 3 + 3 + 4 + 5 + 1 + 32 + 5);
    }
    term_setup();
    r = kernel_printf(edge("a\nbc\n%i\n"), 7);
    expect_row("printf with newlines", 3, "a");
    expect_row("printf with newlines", 4, "bc");
    expect_row("printf with newlines", 5, "7");
    if (r != 4 || terminal_row != 6 || terminal_col != 0)
    {
        BAD("printf with newlines returned %d at (%d,%d), expected 4 at (0,6)", r, terminal_col, terminal_row);
    }
    // Newlines at the bottom scroll.
    term_setup();
    terminal_row = 24;
    r = kernel_printf(edge("x%i\ny"), 1234567890);
    expect_row("printf scrolling", 23, "x1234567890");
    expect_row("printf scrolling", 24, "y");
    if (r != 12 || terminal_row != 24)
    {
        BAD("printf scrolling returned %d on row %d, expected 12 on row 24", r, terminal_row);
    }
}

struct test_case
{
    const char *name;
    void (*run)(void);
    unsigned seconds;
};

static const struct test_case cases[] = {
    { "api", c_api, 10 },
    { "empty", c_empty, 10 },
    { "truncate", c_truncate, 10 },
    { "pad", c_pad, 10 },
    { "limits", c_limits, 10 },
    { "char", c_char, 10 },
    { "string", c_string, 10 },
    { "unsupported", c_unsupported, 10 },
    { "trailing", c_trailing, 10 },
    { "int_max", c_int_max, 60 },
    { "itoa_hex", c_itoa_hex, 10 },
    { "itoa_base", c_itoa_base, 10 },
    { "print_hex", c_print_hex, 10 },
    { "printf_hex", c_printf_hex, 10 },
    { "printf_trailing", c_printf_trailing, 10 },
    { "printf_others", c_printf_others, 10 },
};

int main(int argc, char **argv)
{
    // One page at 0xB8000, the pages next to it unmapped.
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
            alarm(cases[i].seconds);
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
            printf("FAIL %s: stopped by signal %d (%s)\n", cases[i].name, WTERMSIG(status),
                   WTERMSIG(status) == 14 ? "time limit" :
                   WTERMSIG(status) == 11 ? "access outside a buffer, or stack overflow" :
                   WTERMSIG(status) == 8 ? "division error" : "other");
            failed++;
        }
        else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            printf("FAIL %s\n", cases[i].name);
            failed++;
        }
        else
        {
            printf("PASS %s\n", cases[i].name);
        }
    }
    printf("formatting host check: %d of %d cases failed\n", failed, total);
    return failed ? 1 : 0;
}
