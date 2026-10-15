#ifndef TASKSWITCHSEGMENT_H
#define TASKSWITCHSEGMENT_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
When we go back to kernel land the processor
will be looking to the tss and they will pull out the kernel stack pointer on the kernel stack segment
*/

// TSS structure
// Guide the processor in switching the stack back to the correct state when it goes back into kernel land
// This is the 104-byte 32-bit TSS. The segment selector fields (ss0, ss1, ss2, es ... gs, ldtr) are 16 bits
// wide; the upper half of each of these 32-bit slots is reserved and stays 0.
struct tss
{
    uint32_t link;
    uint32_t esp0; // Kernel stack pointer
    uint32_t ss0; // Kernel stack segment
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldtr;
    uint16_t trap; // Bit 0: debug trap on task switch
    uint16_t iomap_base; // Offset of the I/O permission bitmap from the start of the TSS
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "the 32-bit TSS is 104 bytes");
_Static_assert(offsetof(struct tss, esp0) == 4, "TSS esp0 at offset 4");
_Static_assert(offsetof(struct tss, ss0) == 8, "TSS ss0 at offset 8");
_Static_assert(offsetof(struct tss, ss1) == 16, "TSS ss1 at offset 16");
_Static_assert(offsetof(struct tss, esp2) == 20, "TSS esp2 at offset 20");
_Static_assert(offsetof(struct tss, ss2) == 24, "TSS ss2 at offset 24");
_Static_assert(offsetof(struct tss, cr3) == 28, "TSS cr3 at offset 28");
_Static_assert(offsetof(struct tss, eip) == 32, "TSS eip at offset 32");
_Static_assert(offsetof(struct tss, ldtr) == 96, "TSS ldtr at offset 96");
_Static_assert(offsetof(struct tss, trap) == 100, "TSS trap at offset 100");
_Static_assert(offsetof(struct tss, iomap_base) == 102, "TSS I/O map base at offset 102");

void tss_load(int tss_segment);

#ifdef __cplusplus
}
#endif

#endif // !TASKSWITCHSEGMENT_H