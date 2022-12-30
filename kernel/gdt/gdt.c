#include "gdt.h"
#include "kernel.h"

void encodeGdtEntry(uint8_t *targer, struct gdt_structured source)
{
    if ((source.limit > 65536) && (source.limit & 0xFFF) != 0xFFF)
    {
        panic("encodeGdtEntry: Invalid argument\n");
    }

    targer[6] = 0x40;
    if (source.limit > 65536)
    {
        source.limit = source.limit >> 12;
        targer[6] = 0xC0;
    }
    
    // Encodes the limit
    targer[0] = source.limit & 0xFF;
    targer[1] = (source.limit >> 8) & 0xFF;
    targer[6] |= (source.limit >> 16) & 0x0F;

    // Encode the base
    targer[2] = source.base & 0xFF;
    targer[3] = (source.base >> 8) & 0xFF;
    targer[4] = (source.base >> 16) & 0xFF;
    targer[7] = (source.base >> 24) & 0xFF;

    // Set the type
    targer[5] = source.type;

}

// Converts gdt structured into gdt real
void gdt_structured_to_gdt(struct gdt *gdt, struct gdt_structured *structured_gdt, int total_entires)
{
    for (int i = 0; i < total_entires; i++)
    {
        encodeGdtEntry((uint8_t*) &gdt[i], structured_gdt[i]);
    }
}