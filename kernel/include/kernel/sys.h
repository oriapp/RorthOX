#ifndef _KERNEL_SYS_H
#define _KERNEL_SYS_H

#ifdef __cplusplus
extern "C" {
#endif

void IRQ_OFF(void);
void IRQ_RES(void);
void PAUSE(void) __attribute__((noreturn));
// sti; hlt; cli; ret. Call with IF=0 after the last check for work: sti lets maskable interrupts
// in only after the next instruction, so hlt starts and an IRQ already pending ends it at once.
// An NMI is not held back that way: if one comes between sti and hlt, a pending IRQ can run
// before hlt and the halt then waits for the next interrupt (at most one PIT tick).
// Returns with IF=0 once the halt has ended; more than one interrupt may run before the cli.
// The idle loop in task_next is the only caller.
void IRQ_ON_AND_PAUSE(void);
void PANIC(const char *msg, const char *file, int line) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif // !_KERNEL_SYS_H
