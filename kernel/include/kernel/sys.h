#ifndef _KERNEL_SYS_H
#define _KERNEL_SYS_H

void IRQ_OFF(void);
void IRQ_RES(void);
void PAUSE(void);
void IRQ_ON_AND_PAUSE(void);
void PANIC(const char *msg);

#endif // !_KERNEL_SYS_H
