#ifndef LHEAP_H
#define LHEAP_H
#include <stdint.h>
#include <stddef.h>

void kheap_init();
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

#endif // !LHEAP_H