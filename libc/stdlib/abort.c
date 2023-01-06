#include <stdlib.h>
#include <stdio.h>

#define UNUSED(x) (void)(x)

__attribute__((__noreturn__))
void abort(void) {
#if defined(__is_libk)
	// TODO: Add proper kernel panic.
	printf("kernel: panic: abort()\n");
#else
	// TODO: Abnormally terminate the process as if by SIGABRT.
	printf("abort()\n");
#endif
	while (1) { }
	__builtin_unreachable();
}

void panic(const char *msg)
{
	UNUSED(msg);
#if defined(__debug__)
	// TODO: debug panic(??)
#else
	// print system halted
	// IRQ OFF
	// PAUSE
#endif
}