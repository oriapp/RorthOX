#include <stdint.h>
#include "include/kernel/fpu.h"
#include "include/kernel/sys.h"
#include "memory/memory.h"

// CPUID leaf 1 EDX (Intel SDM Vol. 2A, CPUID)
#define CPUID_EDX_FPU (1u << 0)
#define CPUID_EDX_FXSR (1u << 24)
#define CPUID_EDX_SSE (1u << 25)

// Intel SDM Vol. 3A 2.5
#define CR0_MP (1u << 1)  // WAIT/FWAIT see TS
#define CR0_EM (1u << 2)  // clear: x87 runs, SSE doesn't #UD
#define CR0_TS (1u << 3)  // clear: no #NM, state is switched eagerly
#define CR0_NE (1u << 5)  // x87 errors are #MF (vector 16), not IRQ13
#define CR4_OSFXSR (1u << 9)      // FXSAVE/FXRSTOR include XMM and MXCSR, SSE allowed
#define CR4_OSXMMEXCPT (1u << 10) // unmasked SIMD errors are #XM (vector 19), not #UD
#define CR4_OSXSAVE (1u << 18)    // no XSAVE state: stays clear

int fpu_has_cpuid(void);
void fpu_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *edx);
uint32_t fpu_read_cr0(void);
void fpu_write_cr0(uint32_t value);
uint32_t fpu_read_cr4(void);
void fpu_write_cr4(uint32_t value);

// The clean state as data, in the FXSAVE layout (Intel SDM Vol. 2A, FXSAVE): FCW 0x037F at +0
// (what FNINIT sets: every x87 exception masked, extended precision, round to nearest), FSW 0, abridged
// FTW 0 at +4 (all registers empty), MXCSR 0x1F80 at +24 (the reset value: every SIMD exception
// masked, round to nearest), and every ST/MM and XMM register zero. FXRSTOR loads every field,
// so this also clears the register contents FNINIT would leave behind.
static const uint8_t fpu_pristine[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN))) = {
    [0] = 0x7F, [1] = 0x03,
    [24] = 0x80, [25] = 0x1F,
};

void fpu_init(void)
{
    // The feature checks come first: FXSAVE/FXRSTOR #UD without FXSR, and CR4.OSFXSR is
    // reserved without it.
    if (!fpu_has_cpuid())
    {
        PANIC("Unsupported CPU: no CPUID instruction", __FILE__, __LINE__);
    }

    uint32_t max_leaf = 0;
    uint32_t edx = 0;
    fpu_cpuid(0, &max_leaf, &edx);
    if (max_leaf < 1)
    {
        PANIC("Unsupported CPU: no CPUID leaf 1", __FILE__, __LINE__);
    }

    uint32_t signature = 0;
    fpu_cpuid(1, &signature, &edx);
    if (!(edx & CPUID_EDX_FPU))
    {
        PANIC("Unsupported CPU: no x87 FPU (CPUID.1:EDX bit 0)", __FILE__, __LINE__);
    }
    if (!(edx & CPUID_EDX_FXSR))
    {
        PANIC("Unsupported CPU: no FXSAVE/FXRSTOR (CPUID.1:EDX bit 24)", __FILE__, __LINE__);
    }
    if (!(edx & CPUID_EDX_SSE))
    {
        PANIC("Unsupported CPU: no SSE (CPUID.1:EDX bit 25)", __FILE__, __LINE__);
    }

    // Read-modify-write: PE, WP and PG stay as they are.
    uint32_t cr0 = fpu_read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    fpu_write_cr0(cr0);

    uint32_t cr4 = fpu_read_cr4();
    cr4 &= ~CR4_OSXSAVE;
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    fpu_write_cr4(cr4);

    // The CPU now holds the clean state, owned by no task. Loading it here also checks the
    // image once at boot.
    fpu_restore((void *)fpu_pristine);
}

void fpu_task_init(void *area)
{
    memcpy(area, (void *)fpu_pristine, sizeof(fpu_pristine));
}
