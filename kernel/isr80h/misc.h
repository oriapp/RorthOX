#ifndef ISR80H_MISC_H
#define ISR80H_MISC_H

#ifdef __cplusplus
extern "C" {
#endif

struct interrupt_frame;
void *isr80h_command0_sum(struct interrupt_frame *frame);

#ifdef __cplusplus
}
#endif

#endif // !ISR80H_MISC_H