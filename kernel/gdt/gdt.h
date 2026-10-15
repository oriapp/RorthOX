#ifndef GDT_H
#define GDT_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gdt
{
    uint16_t segment;
    uint16_t base_first;
    uint8_t base;
    uint8_t access;
    uint8_t high_flags;
    uint8_t base_24_31_bits;
} __attribute__((packed));

_Static_assert(sizeof(struct gdt) == 8, "a GDT descriptor is 8 bytes");

struct gdt_structured
{
    uint32_t base;
    uint32_t limit;
    uint8_t type;
};

// limit is the GDTR limit: the offset of the last byte of the table (its size - 1)
void gdt_load(struct gdt *gdt, int limit);
void gdt_structured_to_gdt(struct gdt *gdt, struct gdt_structured *structured_gdt, int total_entires);

#ifdef __cplusplus
}
#endif

#endif // !GDT_H