#ifndef _KERNEL_SYS_H
#define _KERNEL_SYS_H

#ifdef __cplusplus
extern "C" {
#endif

void IRQ_OFF(void);
void IRQ_RES(void);
void PAUSE(void) __attribute__((noreturn));
void IRQ_ON_AND_PAUSE(void);
void PANIC(const char *msg, const char *file, int line) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif // !_KERNEL_SYS_H
