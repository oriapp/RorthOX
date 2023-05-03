#ifndef LHEAP_H
#define LHEAP_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void kheap_init();
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

#ifdef __cplusplus
}
#endif

#endif // !LHEAP_H