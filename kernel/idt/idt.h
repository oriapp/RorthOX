#ifndef IDT_H
#define IDT_H
#include <stddef.h>
#include <stdint.h>

struct interrupt_frame;
typedef void*(*ISR80H_COMMAND)(struct interrupt_frame *frame);
typedef void(*INTERRUPT_CALLBACK_FUNCTION)();


struct idt_desc
{
    uint16_t offset_1;  // Offset bits 0 - 15
    uint16_t selector;  // Selector thats in our GDT
    uint8_t zero;       // Does nothing, unused set to zero
    uint8_t type_attr;  // Descriptor tyep and attributes
    uint16_t offset_2;  // Offset bits 16-31
} __attribute__((packed));

_Static_assert(sizeof(struct idt_desc) == 8, "an IDT gate is 8 bytes");

struct idtr_desc
{
    uint16_t limit;     // Size of descriptor table -1
    uint32_t base;      // Base address of the start of the interrupt descriptor table
} __attribute__((packed));

_Static_assert(sizeof(struct idtr_desc) == 6, "the IDTR operand is a 16-bit limit and a 32-bit base");

// What the stubs in idt.asm leave on the stack: pushad, the error code (the CPU's, or 0 from the
// stub), then the CPU's frame. esp and ss are only there when the interrupt came from ring 3; a
// ring 0 frame ends after flags, so don't read them for one.
struct interrupt_frame
{
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t reserved;  // esp saved by pushad, not the user's esp
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t error_code;
    uint32_t ip;
    uint32_t cs;
    uint32_t flags;
    uint32_t esp;
    uint32_t ss;
} __attribute__((packed));

// idt.asm builds the frame in this order
_Static_assert(offsetof(struct interrupt_frame, edi) == 0, "pushad puts edi at 0");
_Static_assert(offsetof(struct interrupt_frame, eax) == 28, "pushad puts eax at 28");
_Static_assert(offsetof(struct interrupt_frame, error_code) == 32, "the stubs put the error code at 32");
_Static_assert(offsetof(struct interrupt_frame, ip) == 36, "the CPU frame starts at 36");
_Static_assert(offsetof(struct interrupt_frame, cs) == 40, "the CPU puts cs at 40");
_Static_assert(offsetof(struct interrupt_frame, flags) == 44, "the CPU puts flags at 44");
_Static_assert(offsetof(struct interrupt_frame, esp) == 48, "the CPU puts the ring 3 esp at 48");
_Static_assert(offsetof(struct interrupt_frame, ss) == 52, "the CPU puts the ring 3 ss at 52");
_Static_assert(sizeof(struct interrupt_frame) == 56, "struct interrupt_frame must stay 56 bytes");

void idt_init();
void enable_interrupts();
void disable_interrupts();
int idt_register_interrupt_callback(int interrupt, INTERRUPT_CALLBACK_FUNCTION interrupt_callback);
void isr80h_register_command(int command_id, ISR80H_COMMAND command);

#endif // !IDT_H