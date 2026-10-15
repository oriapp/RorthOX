/*
 * Host check for scripts/tests/interrupt-count.sh: idt_register_interrupt_callback from the real
 * kernel/idt/idt.c (included here, so its static callback array can be read), built with the host
 * C compiler and AddressSanitizer. Everything else in idt.c is left out by the linker
 * (-ffunction-sections, --gc-sections).
 *
 *   count    the callback array has 256 entries
 *   accept   vectors 0, 1, 0x20, 0x80 and 255 are stored and return 0
 *   reject   -1, 256, 257, 511, 512, INT_MAX and INT_MIN return -EINVARG and change nothing
 *
 * Prints "PASS <case>" or "FAIL <case>" per case, then
 * "interrupt-count host check: <failed> of <cases> cases failed".
 */
#include <limits.h>
#include "idt/idt.c"
#include "host.h"

int host_memcmp(const void *a, const void *b, unsigned long n) __asm__("memcmp");
void *host_memcpy(void *d, const void *s, unsigned long n) __asm__("memcpy");

static void cb_a(void) {}
static void cb_b(void) {}

static int failed, total;

static void report(const char *name, int ok)
{
    total++;
    if (!ok)
    {
        failed++;
    }
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

int main(void)
{
    int n = (int)(sizeof(interrupt_callbacks) / sizeof(interrupt_callbacks[0]));
    if (n != 256)
    {
        printf("  the callback array has %d entries, expected 256\n", n);
    }
    report("count", n == 256);

    static const int good[] = { 0, 1, 0x20, 0x80, 255 };
    int ok = 1;
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
    {
        int v = good[i];
        int r = idt_register_interrupt_callback(v, cb_a);
        if (r != 0 || interrupt_callbacks[v] != cb_a)
        {
            printf("  vector %d: returned %d, stored %s\n", v, r, interrupt_callbacks[v] == cb_a ? "yes" : "no");
            ok = 0;
        }
    }
    report("accept", ok);

    static const int bad[] = { -1, 256, 257, 511, 512, INT_MAX, INT_MIN };
    static INTERRUPT_CALLBACK_FUNCTION before[sizeof(interrupt_callbacks) / sizeof(interrupt_callbacks[0])];
    ok = 1;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        host_memcpy(before, interrupt_callbacks, sizeof(before));
        fflush(0);
        int r = idt_register_interrupt_callback(bad[i], cb_b);
        if (r != -EINVARG)
        {
            printf("  vector %d: returned %d, expected %d\n", bad[i], r, -EINVARG);
            ok = 0;
        }
        if (host_memcmp(before, interrupt_callbacks, sizeof(before)) != 0)
        {
            printf("  vector %d: the callback array changed\n", bad[i]);
            ok = 0;
        }
    }
    report("reject", ok);

    printf("interrupt-count host check: %d of %d cases failed\n", failed, total);
    return failed ? 1 : 0;
}
