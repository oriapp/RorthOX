#ifndef _KERNEL_FPU_H
#define _KERNEL_FPU_H

#ifdef __cplusplus
extern "C" {
#endif

// One task's x87/MMX/SSE state in the 512-byte legacy FXSAVE format. FXSAVE and FXRSTOR raise
// #GP(0) on a memory operand that isn't 16-byte aligned (Intel SDM Vol. 2A, FXSAVE).
#define FPU_STATE_SIZE 512
#define FPU_STATE_ALIGN 16

// Checks CPUID for an x87 FPU, FXSAVE/FXRSTOR and SSE and panics without them. Then sets
// CR0.MP/NE, clears CR0.EM/TS, sets CR4.OSFXSR/OSXMMEXCPT (CR4.OSXSAVE stays clear) and loads the
// clean state with FXRSTOR. Call once with IF=0, after paging is on and before the first task.
void fpu_init(void);
// Fills a task's state area with the clean state: FCW 0x037F, every register empty and zero,
// MXCSR 0x1F80 (all SIMD exceptions masked).
void fpu_task_init(void *area);

// fpu.asm. area is a kernel-owned, 16-byte aligned FPU_STATE_SIZE area. Neither raises #MF:
// a pending x87 exception stays pending until the task's next waiting FP instruction.
void fpu_save(void *area);
void fpu_restore(void *area);

#ifdef __cplusplus
}
#endif

#endif // !_KERNEL_FPU_H
