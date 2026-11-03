#ifndef ISR80H_FILE_H
#define ISR80H_FILE_H

struct interrupt_frame;

void *isr80h_command_file_open(struct interrupt_frame *frame);
void *isr80h_command_file_read(struct interrupt_frame *frame);
void *isr80h_command_file_write(struct interrupt_frame *frame);
void *isr80h_command_file_seek(struct interrupt_frame *frame);
void *isr80h_command_file_stat(struct interrupt_frame *frame);
void *isr80h_command_file_close(struct interrupt_frame *frame);

#endif
