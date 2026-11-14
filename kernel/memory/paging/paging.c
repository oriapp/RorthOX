#include "paging.h"
#include "memory/heap/kheap.h"
#include "status.h"
void paging_load_directory(uint32_t *directory);

static uint32_t *current_directory = 0;
struct paging_4gb_chunk *paging_new_4gb(uint8_t flags)
{
    uint32_t *directory = kzalloc(sizeof(uint32_t) * PAGING_TOTAL_ENTRIES_PER_TABLE);
    if (!directory)
    {
        return NULL;
    }

    int total_tables = 0;
    uint32_t offset = 0;
    for (int i = 0; i < PAGING_TOTAL_ENTRIES_PER_TABLE; i++)
    {
        uint32_t *entry = kzalloc(sizeof(uint32_t) * PAGING_TOTAL_ENTRIES_PER_TABLE);
        if (!entry)
        {
            goto out;
        }

        for (int b = 0; b < PAGING_TOTAL_ENTRIES_PER_TABLE; b++)
        {
            entry[b] = (offset + ((uint32_t)b * PAGING_PAGE_SIZE)) | flags;
        }
        offset += (PAGING_TOTAL_ENTRIES_PER_TABLE * PAGING_PAGE_SIZE);
        directory[i] = (uint32_t)entry | flags | PAGING_IS_WRITEABLE;
        total_tables++;
    }

    struct paging_4gb_chunk *chunk_4gb = kzalloc(sizeof(struct paging_4gb_chunk));
    if (!chunk_4gb)
    {
        goto out;
    }

    chunk_4gb->directory_entry = directory;
    return chunk_4gb;

out:
    for (int i = 0; i < total_tables; i++)
    {
        uint32_t *table = (uint32_t*)(directory[i] & 0xfffff000);
        kfree(table);
    }
    kfree(directory);
    return NULL;
}

void paging_switch(struct paging_4gb_chunk *directory)
{
    paging_load_directory(directory->directory_entry);
    current_directory = directory->directory_entry;
}

void paging_free_4gb(struct paging_4gb_chunk *chunk)
{
    for (int i = 0; i < 1024; i++)
    {
        uint32_t entry = chunk->directory_entry[i];
        uint32_t *table = (uint32_t*)(entry & 0xfffff000); // actual page table address
        // This is also why the page tables are aligned
        kfree(table);
    }

    kfree(chunk->directory_entry);
    kfree(chunk);
}

uint32_t *paging_4gb_chunk_get_directory(struct paging_4gb_chunk *chunk)
{
    return chunk->directory_entry;
}

bool paging_is_aligned(void *addr)
{
    return ((uint32_t)addr % PAGING_PAGE_SIZE) == 0;
}

int paging_get_indexes(void *virtual_address, uint32_t *directory_index_out, uint32_t *table_index_out)
{
    int res = 0;
    if (!paging_is_aligned(virtual_address))
    {
        res = -EINVARG;
        goto out;
    }

    // Calculate directory index
    *directory_index_out = ((uint32_t)virtual_address / (PAGING_TOTAL_ENTRIES_PER_TABLE * PAGING_PAGE_SIZE));
    *table_index_out = ((uint32_t)virtual_address % (PAGING_TOTAL_ENTRIES_PER_TABLE * PAGING_PAGE_SIZE) / PAGING_PAGE_SIZE);
out:
    return res;
}

void *paging_align_address(void *ptr)
{
    if ((uint32_t)ptr % PAGING_PAGE_SIZE)
    {
        return (void*)((uint32_t)ptr + PAGING_PAGE_SIZE - ((uint32_t)ptr % PAGING_PAGE_SIZE));
    }

    return ptr;
}

void *paging_align_to_lower_page(void *addr)
{
    uint32_t _addr = (uint32_t)addr;
    _addr -= (_addr % PAGING_PAGE_SIZE);
    return (void *)_addr;
}

// A leaf may only be written in an inactive directory, through a present PDE that points to a
// 4 KiB page table (PS clear). Checked before the table address is followed, whatever the new
// PTE value is: a missing or 4 MiB PDE holds no table address to write through.
static int paging_check_leaf(uint32_t *directory, uint32_t virt)
{
    if (!directory || directory == current_directory || (virt % PAGING_PAGE_SIZE))
    {
        return -EINVARG;
    }

    uint32_t entry = directory[virt >> 22];
    if (!(entry & PAGING_IS_PRESENT) || (entry & PAGING_PAGE_SIZE_FLAG))
    {
        return -EINVARG;
    }

    return 0;
}

// Writes a leaf that paging_check_leaf accepted; cannot fail.
static void paging_write_leaf(uint32_t *directory, uint32_t virt, uint32_t val)
{
    uint32_t user = PAGING_IS_PRESENT | PAGING_ACCESS_FROM_ALL;
    if ((val & user) == user)
    {
        // Effective user access still requires U in the individual leaf. Never demoted here:
        // other user leaves may share the PDE.
        directory[virt >> 22] |= PAGING_ACCESS_FROM_ALL;
    }

    uint32_t *table = (uint32_t *)(directory[virt >> 22] & 0xFFFFF000);
    table[(virt >> 12) & 0x3FF] = val;
}

int paging_map(struct paging_4gb_chunk *directory, void *virt, void *phys, int flags)
{
    return paging_map_range(directory, virt, phys, 1, flags);
}

// Every leaf and the address arithmetic are checked before the first write, so a failure
// changes no PDE or PTE. This holds only because nothing else touches the directory between
// the check and the writes: IF=0, one CPU, and the directory is not loaded.
int paging_map_range(struct paging_4gb_chunk *directory, void *virt, void *phys, int count, int flags)
{
    uint32_t v = (uint32_t)virt;
    uint32_t p = (uint32_t)phys;
    if (!directory || !directory->directory_entry || directory->directory_entry == current_directory)
    {
        return -EINVARG;
    }

    // Flags above bit 11 would land in the frame address.
    if (count < 0 || (v % PAGING_PAGE_SIZE) || (p % PAGING_PAGE_SIZE) || ((uint32_t)flags & 0xFFFFF000))
    {
        return -EINVARG;
    }

    // An empty range maps nothing.
    if (count == 0)
    {
        return 0;
    }

    // The last page must not wrap past 0xFFFFF000 in either address space.
    uint32_t last = (uint32_t)count - 1;
    if (last > ((0xFFFFF000 - v) / PAGING_PAGE_SIZE) || last > ((0xFFFFF000 - p) / PAGING_PAGE_SIZE))
    {
        return -EINVARG;
    }

    uint32_t *entries = directory->directory_entry;
    for (uint32_t i = 0; i <= last; i++)
    {
        int res = paging_check_leaf(entries, v + i * PAGING_PAGE_SIZE);
        if (res < 0)
        {
            return res;
        }
    }

    for (uint32_t i = 0; i <= last; i++)
    {
        paging_write_leaf(entries, v + i * PAGING_PAGE_SIZE, (p + i * PAGING_PAGE_SIZE) | (uint32_t)flags);
    }

    return 0;
}

int paging_map_to(struct paging_4gb_chunk *directory, void *virt, void *phys, void *phys_end, int flags)
{
    int res = 0;
    if ((uint32_t)virt % PAGING_PAGE_SIZE)
    {
        res = -EINVARG;
        goto out;
    }

    if ((uint32_t)phys % PAGING_PAGE_SIZE)
    {
        res = -EINVARG;
        goto out;
    }

    if ((uint32_t)phys_end % PAGING_PAGE_SIZE)
    {
        res = -EINVARG;
        goto out;
    }

    if ((uint32_t)phys_end < (uint32_t)phys)
    {
        res = -EINVARG;
        goto out;
    }

    uint32_t total_bytes = (uint32_t)phys_end - (uint32_t)phys;
    int total_pages = total_bytes / PAGING_PAGE_SIZE;
    res = paging_map_range(directory, virt, phys, total_pages, flags);

out:
    return res;
}

int paging_set(uint32_t *directory, void *virt, uint32_t val)
{
    int res = paging_check_leaf(directory, (uint32_t)virt);
    if (res < 0)
    {
        return res;
    }

    paging_write_leaf(directory, (uint32_t)virt, val);
    return 0;
}

// Unchecked translation: not a user-pointer validation or copy path.
void *paging_get_physical_address(uint32_t *directory, void *virt)
{
    void *virt_addr_new = (void*) paging_align_to_lower_page(virt);
    void *difference = (void*)((uint32_t) virt - (uint32_t) virt_addr_new);
    return (void*)((paging_get(directory, virt_addr_new) & 0xFFFFF000) + difference);
}

int paging_user_translate(uint32_t *directory, uint32_t address, int write, void **physical)
{
    if (!directory || !physical)
    {
        return -EINVARG;
    }

    // Both levels must permit user access; only 4 KiB page tables are supported.
    uint32_t required = PAGING_IS_PRESENT | PAGING_ACCESS_FROM_ALL;
    if (write)
    {
        required |= PAGING_IS_WRITEABLE;
    }
    uint32_t entry = directory[address >> 22];
    if ((entry & required) != required || (entry & PAGING_PAGE_SIZE_FLAG))
    {
        return -EINVARG;
    }

    uint32_t *table = (uint32_t*)(entry & 0xFFFFF000);
    entry = table[(address >> 12) & 0x3FF];
    if ((entry & required) != required)
    {
        return -EINVARG;
    }

    *physical = (void*)((entry & 0xFFFFF000) | (address & (PAGING_PAGE_SIZE - 1)));
    return 0;
}

uint32_t paging_get(uint32_t *directory, void *virt)
{
    uint32_t directory_index = 0;
    uint32_t table_index = 0;
    paging_get_indexes(virt, &directory_index, &table_index);

    uint32_t entry = directory[directory_index];
    uint32_t *table = (uint32_t *)(entry & 0xFFFFF000);
    return table[table_index];
}

/*
Resources:
    https://wiki.osdev.org/Paging
    https://wiki.osdev.org/Setting_Up_Paging
    https://wiki.osdev.org/Page_Tables
    https://wiki.osdev.org/Identity_Paging

    https://wiki.osdev.org/Setting_Up_Paging_With_PAE
*/