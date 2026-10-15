/*
 * Host check for scripts/tests/descriptor-tables.sh: the real kernel/gdt/gdt.c encoder, built with
 * the host C compiler, and the GDT/TSS structures of kernel/gdt/gdt.h and kernel/task/tss.h.
 *
 *   flat     the kernel's code and data descriptors (base 0, limit 0xffffffff, access 0x9A, 0x92,
 *            0xF8, 0xF2) and the null descriptor encode to the same bytes as before:
 *            ff ff 00 00 00 <access> cf 00 (4 KiB granularity, 32-bit, limit 0xfffff) and
 *            00 00 00 00 00 00 40 00
 *   tss      a TSS descriptor (access 0xE9, limit 0x67) at a distinctive base: base, limit 0x67,
 *            byte granularity, present, DPL 3, type 9 (32-bit TSS, available) and bit 54 (the
 *            reserved bit 6 of byte 6, the D/B bit of code and data descriptors) clear
 *   tss_dpl0 the same with access 0x89 (DPL 0): bit 54 clear as well
 *   sizes    struct gdt is 8 bytes, struct tss is 104 bytes
 *
 * Usage: descriptor-tables
 * Prints "PASS <case>" or "FAIL <case>: ..." per case, then
 * "descriptor-tables host check: <failed> of <cases> cases failed".
 */
#include <stdint.h>
#include <stddef.h>
#include "gdt/gdt.h"
#include "task/tss.h"
#include "host.h"

static int failed, total;

// gdt.c calls PANIC for limits it can't encode; none of the inputs here should get there.
void PANIC(const char *msg, const char *file, int line)
{
    printf("  PANIC from the encoder: %s", msg);
    _exit(3);
}

static void report(const char *name, int ok, const char *detail, const uint8_t *got)
{
    total++;
    if (ok)
    {
        printf("PASS %s\n", name);
        return;
    }
    failed++;
    printf("FAIL %s: %s", name, detail);
    if (got)
    {
        printf(" (got");
        for (int i = 0; i < 8; i++)
        {
            printf(" %02x", got[i]);
        }
        printf(")");
    }
    printf("\n");
}

static void encode(uint8_t *out, uint32_t base, uint32_t limit, uint8_t type)
{
    struct gdt_structured s = { .base = base, .limit = limit, .type = type };
    for (int i = 0; i < 8; i++)
    {
        out[i] = 0x5A;
    }
    encodeGdtEntry(out, s);
}

static int same(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 8; i++)
    {
        if (a[i] != b[i])
        {
            return 0;
        }
    }
    return 1;
}

static void tss_case(const char *name, uint8_t access)
{
    uint8_t d[8];
    const uint32_t base = 0x12345678;
    encode(d, base, sizeof(struct tss) - 1, access);
    uint32_t got_base = d[2] | d[3] << 8 | d[4] << 16 | (uint32_t)d[7] << 24;
    uint32_t got_limit = d[0] | d[1] << 8 | (d[6] & 0x0F) << 16;
    int ok = 1;
    if (got_base != base)
    {
        ok = 0;
        printf("  %s: base 0x%x, expected 0x%x\n", name, got_base, base);
    }
    if (got_limit != 0x67)
    {
        ok = 0;
        printf("  %s: limit 0x%x, expected 0x67\n", name, got_limit);
    }
    if (d[5] != access)
    {
        ok = 0;
        printf("  %s: access byte 0x%02x, expected 0x%02x (present, DPL %d, type 9)\n", name, d[5], access,
               (access >> 5) & 3);
    }
    if (d[6] & 0x80)
    {
        ok = 0;
        printf("  %s: granularity bit set (limit in 4 KiB units)\n", name);
    }
    if (d[6] & 0x40)
    {
        ok = 0;
        printf("  %s: bit 54 (reserved for a TSS, D/B for code and data) is set\n", name);
    }
    if (d[6] & 0x30)
    {
        ok = 0;
        printf("  %s: bits 52-53 set (byte 6 is 0x%02x)\n", name, d[6]);
    }
    report(name, ok, "see above", d);
}

int main(void)
{
    static const uint8_t accesses[] = { 0x9A, 0x92, 0xF8, 0xF2 };
    uint8_t d[8];
    int ok = 1;
    for (int i = 0; i < 4; i++)
    {
        const uint8_t want[8] = { 0xff, 0xff, 0x00, 0x00, 0x00, accesses[i], 0xcf, 0x00 };
        encode(d, 0, 0xffffffff, accesses[i]);
        if (!same(d, want))
        {
            ok = 0;
            printf("  access 0x%02x: %02x %02x %02x %02x %02x %02x %02x %02x, expected %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   accesses[i], d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                   want[0], want[1], want[2], want[3], want[4], want[5], want[6], want[7]);
        }
    }
    const uint8_t null_want[8] = { 0, 0, 0, 0, 0, 0, 0x40, 0 };
    encode(d, 0, 0, 0);
    if (!same(d, null_want))
    {
        ok = 0;
        printf("  null descriptor: %02x %02x %02x %02x %02x %02x %02x %02x, expected 00 00 00 00 00 00 40 00\n",
               d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    report("flat", ok, "code, data or null descriptor bytes changed", 0);

    tss_case("tss", 0xE9);
    tss_case("tss_dpl0", 0x89);

    int sizes_ok = sizeof(struct gdt) == 8 && sizeof(struct tss) == 104;
    if (!sizes_ok)
    {
        printf("  sizeof(struct gdt) %d (want 8), sizeof(struct tss) %d (want 104)\n", (int)sizeof(struct gdt),
               (int)sizeof(struct tss));
    }
    report("sizes", sizes_ok, "see above", 0);

    printf("descriptor-tables host check: %d of %d cases failed\n", failed, total);
    return failed ? 1 : 0;
}
