/*
 * Host check for scripts/tests/rtc-helpers.sh: the real kernel/time/rtc/rtc.h (getformattedtime,
 * gettime, printtime, read_rtc) with the real kernel/string/string.c, built with the host C
 * compiler. string.c's formatting names get a kernel_ prefix (as in formatting.sh), so no call can
 * end up in the C library's formatter; the expected text comes from the C library's snprintf.
 *
 * The heap is scripts/tests/host/kalloc.c: kmalloc memory is filled with 0x5A (not zero), and the
 * result of every call gets pages of its own that end right before an unmapped page when it has
 * the size the text needs (length + 1), so a store or read past it stops the case with SIGSEGV.
 * Format strings are copied to the end of a page too: reading past their NUL also stops the case.
 * Every case runs in its own process with a time limit.
 *
 * getformattedtime(format): h m s = hour, minute, second as two digits with a leading zero;
 * H M S = the same with a leading space below 10; every other byte is copied as it is. Any token
 * may repeat, so the text can be longer than the format. Returns an allocation of at least
 * length + 1 bytes holding the terminated text, and nothing else stays allocated; NULL when an
 * allocation fails, with nothing left allocated.
 * gettime(): "%d:%d:%d" in an allocation of at least length + 1 bytes; NULL when the allocation fails.
 * printtime(): reads the CMOS clock (a fake one on ports 0x70/0x71 here) and prints
 * "System Time: " in light green, then "HH:MM:SS" with leading zeros and a newline in the default
 * colour; it allocates nothing.
 *
 * Usage: rtc-helpers [case...]   (no argument: every case)
 * Prints "PASS <case>" or "FAIL <case>" per case, then
 * "rtc helpers host check: <failed> of <cases> cases failed".
 */
#include <stdint.h>
#include <stddef.h>

#include "kalloc.h"
#include "time/rtc/rtc.h"
#include "host.h"

// The C library's snprintf, for the expected text.
int host_snprintf(char *str, unsigned long size, const char *format, ...) __asm__("snprintf");
void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
int mprotect(void *addr, unsigned long length, int prot);
int strcmp(const char *a, const char *b);
unsigned int alarm(unsigned int seconds);

#define PAGE 4096UL

static int problems;

#define BAD(...) do { if (problems++ < 12) { printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

// --- fake CMOS and screen -----------------------------------------------------------------------

static unsigned char cmos[128];
static unsigned char cmos_index;

void outb(unsigned short port, unsigned char val)
{
    if (port == 0x70)
    {
        cmos_index = val & 0x7F;
    }
}

unsigned char insb(unsigned short port)
{
    return port == 0x71 ? cmos[cmos_index] : 0xFF;
}

#define DEFAULT_COLOUR 0x100   // print(): the terminal's own colour
static char screen_text[256];
static unsigned screen_colour[256];
static int screen_len;

static void screen_add(const char *str, unsigned colour)
{
    for (; *str; str++)
    {
        if (screen_len >= (int)sizeof(screen_text) - 1)
        {
            BAD("more than %d characters printed", (int)sizeof(screen_text) - 1);
            return;
        }
        screen_colour[screen_len] = colour;
        screen_text[screen_len++] = *str;
        screen_text[screen_len] = 0;
    }
}

void print(const char *str)
{
    screen_add(str, DEFAULT_COLOUR);
}

void printc(const char *str, unsigned int color)
{
    screen_add(str, color);
}

// --- guarded memory -----------------------------------------------------------------------------

// A copy of <s> whose NUL is the last readable byte.
static const char *edge(const char *s)
{
    unsigned long n = 0;
    while (s[n])
    {
        n++;
    }
    n++;
    unsigned long pages = (n + PAGE - 1) / PAGE + 1;
    char *m = mmap(0, (pages + 1) * PAGE, 3, 0x22, -1, 0);
    if (m == (char *)-1)
    {
        printf("  mmap failed\n");
        _exit(3);
    }
    mprotect(m + pages * PAGE, PAGE, 0);
    char *p = m + pages * PAGE - n;
    for (unsigned long i = 0; i < n; i++)
    {
        p[i] = s[i];
    }
    return p;
}

static void show(char *out, unsigned long cap, const char *s, long n)
{
    unsigned long j = 0;
    for (long i = 0; i < n && j + 8 < cap && j < 70; i++)
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

static void set_time(int h, int m, int s)
{
    hour = h;
    minute = m;
    second = s;
}

// The text getformattedtime must give, from the C library's formatter.
static int expected_format(const char *format, char *out, unsigned long cap)
{
    unsigned long n = 0;
    for (const char *p = format; *p; p++)
    {
        int v = -1, zero = 0;
        switch (*p)
        {
        case 'h': v = hour; zero = 1; break;
        case 'H': v = hour; break;
        case 'm': v = minute; zero = 1; break;
        case 'M': v = minute; break;
        case 's': v = second; zero = 1; break;
        case 'S': v = second; break;
        }
        if (v < 0)
        {
            if (n + 1 >= cap)
            {
                return -1;
            }
            out[n++] = *p;
            out[n] = 0;
            continue;
        }
        int k = host_snprintf(out + n, cap - n, zero ? "%02d" : "%2d", v);
        if (k < 0 || n + k >= cap)
        {
            return -1;
        }
        n += k;
    }
    out[n] = 0;
    return (int)n;
}

// Check an allocated result <r> against <want> (<n> characters), then free it.
static void check_allocated(const char *what, char *r, const char *want, int n)
{
    char a[96], b[96];
    if (!r)
    {
        BAD("%s: returned NULL", what);
        return;
    }
    long size = kalloc_size(r);
    if (size < 0)
    {
        BAD("%s: the result is not the start of a live allocation", what);
        return;
    }
    if (size < n + 1)
    {
        show(a, sizeof(a), r, size);
        BAD("%s: allocation of %ld bytes for %d characters and the NUL (holds \"%s\")", what, size, n, a);
        return;
    }
    for (int i = 0; i < n; i++)
    {
        if (r[i] != want[i])
        {
            show(a, sizeof(a), r, n);
            show(b, sizeof(b), want, n);
            BAD("%s: \"%s\", expected \"%s\"", what, a, b);
            return;
        }
    }
    if (r[n] != 0)
    {
        BAD("%s: no NUL after %d characters (0x%02x)", what, n, (unsigned char)r[n]);
    }
    if (kalloc_live() != 1)
    {
        BAD("%s: %d allocations live, expected only the result", what, kalloc_live());
    }
    kfree(r);
    if (kalloc_bad_frees() || kalloc_poison_errors())
    {
        BAD("%s: %d bad frees, %d writes after free", what, kalloc_bad_frees(), kalloc_poison_errors());
    }
}

static void format_case(const char *format)
{
    static char want[16384];
    char what[96];
    int n = expected_format(format, want, sizeof(want));
    if (n < 0)
    {
        BAD("test format too long");
        return;
    }
    show(what, 60, format, strlen(format) < 40 ? strlen(format) : 40);
    host_snprintf(what + strlen(what), 36, " at %02d:%02d:%02d", hour, minute, second);
    kalloc_init();
    // The exact size gets guard pages; any other size still has to hold the text.
    kalloc_guard_next(n + 1, KALLOC_GUARD_ABOVE);
    check_allocated(what, getformattedtime(edge(format)), want, n);
}

static const int times[][3] = {
    { 0, 0, 0 }, { 1, 2, 3 }, { 9, 5, 7 }, { 10, 10, 10 }, { 12, 34, 56 }, { 23, 59, 59 }, { 0, 9, 10 }, { 7, 30, 0 },
};
#define TIMES (sizeof(times) / sizeof(times[0]))

// --- cases --------------------------------------------------------------------------------------

// Every token alone and in the usual clock formats.
static void c_tokens(void)
{
    static const char *const formats[] = {
        "h", "H", "m", "M", "s", "S", "h:m:s", "H:M:S", "hms", "HMS", "h:M", "H.m.S",
    };
    for (unsigned t = 0; t < TIMES; t++)
    {
        set_time(times[t][0], times[t][1], times[t][2]);
        for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        {
            format_case(formats[i]);
        }
    }
}

// Repeated tokens make the text longer than the format (up to twice its length).
static void c_repeat(void)
{
    static char big[4097];
    for (unsigned t = 0; t < TIMES; t++)
    {
        set_time(times[t][0], times[t][1], times[t][2]);
        format_case("hh");
        format_case("HHHHHHHHHHHHHHHH");
        format_case("ssssssss");
        format_case("MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM");
        format_case("hmsHMShmsHMShmsHMShmsHMShmsHMShmsHMShmsHMShmsHMShmsHMShmsHMShmsHMShmsHMShmsHMS");
        format_case("h:h:h:h:m:m:m:m:s:s:s:s");
    }
    // 4096 tokens: 8192 characters.
    for (int i = 0; i < 4096; i++)
    {
        big[i] = "hmsHMS"[i % 6];
    }
    big[4096] = 0;
    set_time(4, 5, 6);
    format_case(big);
}

// Bytes that are not tokens are copied as they are, % and backslashes too.
static void c_literals(void)
{
    set_time(8, 9, 1);
    format_case("x");
    format_case("at h:m:s!");
    format_case("%d%s%%%n%");
    format_case("\\n\t\x01\x7f\x80\xff");
    format_case("abcdefgijklnopqrtuvwxyz ABCDEFGIJKLNOPQRTUVWXYZ 0123456789");
    format_case("[H] (M) {S}");
    format_case("::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::");
}

// An empty format gives an empty, terminated, allocated string.
static void c_empty(void)
{
    for (unsigned t = 0; t < 2; t++)
    {
        set_time(times[t][0], times[t][1], times[t][2]);
        format_case("");
    }
}

// A failed allocation gives NULL with nothing written and nothing left allocated, whichever
// allocation fails.
static void c_alloc_fail(void)
{
    static const char *const formats[] = { "", "h:m:s", "HHHHHHHHHHHHHHHH", "at h:m:s!" };
    set_time(1, 2, 3);
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
    {
        kalloc_init();
        char *r = getformattedtime(edge(formats[i]));
        int calls = kalloc_calls();
        if (r)
        {
            kfree(r);
        }
        if (calls < 1)
        {
            BAD("getformattedtime(\"%s\") allocated nothing", formats[i]);
        }
        for (int k = 1; k <= calls; k++)
        {
            kalloc_init();
            kalloc_fail_at(k);
            r = getformattedtime(edge(formats[i]));
            if (r)
            {
                BAD("getformattedtime(\"%s\"), allocation %d of %d failed: returned a string", formats[i], k, calls);
            }
            if (kalloc_live() || kalloc_bad_frees() || kalloc_poison_errors())
            {
                BAD("getformattedtime(\"%s\"), allocation %d of %d failed: %d live, %d bad frees, %d writes after free",
                    formats[i], k, calls, kalloc_live(), kalloc_bad_frees(), kalloc_poison_errors());
            }
        }
    }
    kalloc_init();
    char *r = gettime();
    int calls = kalloc_calls();
    if (r)
    {
        kfree(r);
    }
    for (int k = 1; k <= calls; k++)
    {
        kalloc_init();
        kalloc_fail_at(k);
        r = gettime();
        if (r || kalloc_live() || kalloc_bad_frees() || kalloc_poison_errors())
        {
            BAD("gettime, allocation %d of %d failed: returned %s, %d live, %d bad frees, %d writes after free",
                k, calls, r ? "a string" : "NULL", kalloc_live(), kalloc_bad_frees(), kalloc_poison_errors());
        }
    }
}

static void c_gettime(void)
{
    static const int more[][3] = { { 255, 255, 255 }, { 99, 100, 101 } };
    for (unsigned t = 0; t < TIMES + 2; t++)
    {
        const int *v = t < TIMES ? times[t] : more[t - TIMES];
        char want[64], what[64];
        set_time(v[0], v[1], v[2]);
        int n = host_snprintf(want, sizeof(want), "%d:%d:%d", v[0], v[1], v[2]);
        host_snprintf(what, sizeof(what), "gettime at %d:%d:%d", v[0], v[1], v[2]);
        kalloc_init();
        kalloc_guard_next(n + 1, KALLOC_GUARD_ABOVE);
        check_allocated(what, gettime(), want, n);
    }
}

// printtime reads the fake CMOS: register B bit 1 = 24-hour clock, bit 2 = binary (else BCD).
static void c_printtime(void)
{
    static const struct { unsigned char b, h, m, s; const char *want; } cases[] = {
        { 0x02, 0x01, 0x02, 0x03, "01:02:03" },
        { 0x02, 0x00, 0x00, 0x00, "00:00:00" },
        { 0x02, 0x23, 0x59, 0x09, "23:59:09" },
        { 0x02, 0x10, 0x05, 0x50, "10:05:50" },
        { 0x06, 7, 8, 9, "07:08:09" },
        { 0x06, 0, 0, 59, "00:00:59" },
        { 0x00, 0x81, 0x05, 0x00, "13:05:00" },   // 12-hour clock, BCD, PM bit
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char want[64];
        host_snprintf(want, sizeof(want), "System Time: %s\n", cases[i].want);
        for (unsigned r = 0; r < sizeof(cmos); r++)
        {
            cmos[r] = 0;
        }
        cmos[0x00] = cases[i].s;
        cmos[0x02] = cases[i].m;
        cmos[0x04] = cases[i].h;
        cmos[0x07] = 0x15;
        cmos[0x08] = 0x06;
        cmos[0x09] = 0x26;
        cmos[0x0A] = 0x26;   // update not in progress
        cmos[0x0B] = cases[i].b;
        set_time(77, 77, 77);
        screen_len = 0;
        screen_text[0] = 0;
        kalloc_init();
        printtime();
        if (strcmp(screen_text, want) != 0)
        {
            char a[96];
            show(a, sizeof(a), screen_text, screen_len);
            BAD("printtime of %s printed \"%s\"", cases[i].want, a);
            continue;
        }
        for (int k = 0; k < screen_len; k++)
        {
            unsigned c = k < 13 ? VGA_COLOR_LIGHT_GREEN : DEFAULT_COLOUR;
            if (screen_colour[k] != c)
            {
                BAD("printtime of %s: character %d in colour %#x, expected %#x", cases[i].want, k, screen_colour[k], c);
                break;
            }
        }
        if (kalloc_calls() != 0)
        {
            BAD("printtime of %s made %d heap allocations", cases[i].want, kalloc_calls());
        }
    }
}

struct test_case
{
    const char *name;
    void (*run)(void);
    unsigned seconds;
};

static const struct test_case cases[] = {
    { "tokens", c_tokens, 20 },
    { "repeat", c_repeat, 20 },
    { "literals", c_literals, 10 },
    { "empty", c_empty, 10 },
    { "alloc_fail", c_alloc_fail, 10 },
    { "gettime", c_gettime, 10 },
    { "printtime", c_printtime, 10 },
};

int main(int argc, char **argv)
{
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
                   WTERMSIG(status) == 11 ? "access outside a buffer, or a NULL pointer" : "other");
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
    printf("rtc helpers host check: %d of %d cases failed\n", failed, total);
    return failed ? 1 : 0;
}
