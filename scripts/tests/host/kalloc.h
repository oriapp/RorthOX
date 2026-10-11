/*
 * Host stand-in for the kernel heap, used by the host checks in scripts/tests/host/.
 *
 * kzalloc/kmalloc/kfree hand out memory from one area below 2 GiB, so a pointer survives the
 * kernel's (int) casts in ISERR()/ERROR_I() on a 64-bit host. Freed memory is never reused: it is
 * filled with KALLOC_POISON, and kalloc_poison_errors() finds later writes to it. Every call is
 * counted, and kalloc_fail_at(n) makes the n-th allocation call return NULL.
 */
#ifndef HOST_KALLOC_H
#define HOST_KALLOC_H

#include <stddef.h>

#define KALLOC_POISON 0xA5

void kalloc_init(void);
// Make allocation call number n (counted from 1 since kalloc_init) return NULL. 0 = never.
void kalloc_fail_at(int n);
// 1 if the chosen allocation call happened (and returned NULL).
int kalloc_failed(void);
// Allocation calls so far (kzalloc and kmalloc).
int kalloc_calls(void);
// Allocations not freed yet.
int kalloc_live(void);
// 1 if ptr is the start of a live allocation.
int kalloc_is_live(const void *ptr);
// Size of the live allocation at ptr, -1 if there is none.
long kalloc_size(const void *ptr);
// Live allocation number i (0 = oldest live one), NULL past the end.
void *kalloc_live_at(int i);
// Frees of something that isn't a live allocation (double free, bad pointer).
int kalloc_bad_frees(void);
// Freed allocations that were written to after kfree.
int kalloc_poison_errors(void);
// Name of the function that made allocation call n (needs -rdynamic), or "?".
const char *kalloc_caller(int n);
// Fill the next <bytes> bytes after the last allocation with copies of <pattern> (<size> bytes),
// like stale data that a read past the end of an allocation would see. Call after kalloc_init().
void kalloc_fill_unused(const void *pattern, size_t size, size_t bytes);

void *kzalloc(size_t size);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
