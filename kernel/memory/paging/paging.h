#ifndef PAGING_H
#define PAGING_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAGING_CACHE_DISABLED 0b00010000  // C - Cache Disables
#define PAGING_WRITE_THROUGH 0b00001000   // W - Write Through
#define PAGING_ACCESS_FROM_ALL 0b00000100 // U - User/Supervisor
#define PAGING_IS_WRITEABLE 0b00000010    // R/W - Read/Write
#define PAGING_IS_PRESENT 0b00000001      // P - Present

#define PAGING_TOTAL_ENTRIES_PER_TABLE 1024
#define PAGING_PAGE_SIZE 4096

struct paging_4gb_chunk
{
    uint32_t *directory_entry;
};

struct paging_4gb_chunk *paging_new_4gb(uint8_t flags);
void paging_switch(struct paging_4gb_chunk *directory);
void enable_paging();

int paging_set(uint32_t *directory, void *virt, uint32_t val);
bool paging_is_aligned(void *addr);

uint32_t *paging_4gb_chunk_get_directory(struct paging_4gb_chunk *chunk);
void paging_free_4gb(struct paging_4gb_chunk *chunk);

int paging_map_to(struct paging_4gb_chunk *directory, void *virt, void *phys, void *phys_end, int flags);
int paging_map_range(struct paging_4gb_chunk *directory, void *virt, void *phys, int count, int flags);
int paging_map(struct paging_4gb_chunk *direcotry, void *virt, void *phys, int flags);
void *paging_align_to_lower_page(void *addr);
void *paging_align_address(void *ptr);
void *paging_get_physical_address(uint32_t *directory, void *virt);
uint32_t paging_get(uint32_t *direcotry, void *virt);

#ifdef __cplusplus
}
#endif

#endif // !PAGING_H