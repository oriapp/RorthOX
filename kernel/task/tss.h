#ifndef TASKSWITCHSEGMENT_H
#define TASKSWITCHSEGMENT_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
When we go back to kernel land the processor
will be looking to the tss and they will pull out the kernel stack pointer on the kernel stack segment
*/

// TSS structure
// Guide the processor in switching the stack back to the correct state when it goes back into kernel land
struct tss
{
    uint32_t link;
    uint32_t esp0; // Kernel stack pointer
    uint32_t ss0; // Kernel stack segment
    uint32_t esp1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t sr3;
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
    uint32_t iopb;
} __attribute__((packed));

void tss_load(int tss_segment);

#ifdef __cplusplus
}
#endif

#endif // !TASKSWITCHSEGMENT_H