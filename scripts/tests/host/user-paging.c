/* Exercise the real page-table walk, not process-host's mapping model. */
#include "host.h"
#include "kalloc.h"
#include "memory/paging/paging.h"
#include "status.h"
extern int paging_user_translate(uint32_t *, uint32_t, int, void **) __attribute__((weak));
void paging_load_directory(uint32_t *directory) {}
int main(void)
{
    if (!paging_user_translate) { printf("user-paging: checked translation API absent\n"); return 1; }
    kalloc_init(); kalloc_set_alignment(4096);
    uint32_t *directory = kzalloc(4096), *table = kzalloc(4096);
    int failed = 0, cases = 0;
    /* All combinations of PDE/PTE P,U,W, both access types. A rejected PDE
       deliberately points to inaccessible memory: it must be checked first. */
    for (int write = 0; write < 2; write++)
    for (int d = 0; d < 8; d++)
    for (int t = 0; t < 8; t++)
    {
        int required = write ? 7 : 5;
        directory[123] = ((d & required) == required ? (uint32_t)(unsigned long)table : 0xfffff000U) | d;
        table[456] = 0x12345000 | t;
        uint32_t before_directory = directory[123], before_table = table[456];
        void *out = (void *)0x1234;
        int rc = paging_user_translate(directory, (123U<<22) | (456U<<12) | 4093, write, &out);
        int good = (d & required) == required && (t & required) == required;
        if (rc != (good ? 0 : -EINVARG) || out != (good ? (void *)0x12345ffd : (void *)0x1234)) failed++;
        if (directory[123] != before_directory || table[456] != before_table) failed++;
        cases++;
    }
    /* PAT and software-available bits do not restrict a 4 KiB mapping. */
    for (int write = 0; write < 2; write++)
    for (int flags = 0; flags < 3; flags++)
    {
        uint32_t extra_directory = flags == 1 ? 0xe00 : 0;
        uint32_t extra_table = flags == 0 ? 0x80 : flags == 2 ? 0xe00 : 0;
        directory[123] = (uint32_t)(unsigned long)table | 7 | extra_directory;
        table[456] = 0x12345007 | extra_table;
        uint32_t before_directory = directory[123], before_table = table[456];
        void *out = (void *)0x1234;
        int rc = paging_user_translate(directory, (123U<<22) | (456U<<12) | 4093, write, &out);
        if (rc || out != (void *)0x12345ffd) failed++;
        if (directory[123] != before_directory || table[456] != before_table) failed++;
        cases++;
    }
    table[0] = 0x12345007;
    directory[123] = (uint32_t)(unsigned long)table | 0x87;
    void *out = (void *)0x1234;
    failed += paging_user_translate(directory, 123U<<22, 0, &out) != -EINVARG || out != (void *)0x1234;
    printf("user-paging: %d of %d failed\n", failed, cases+1);
    return failed != 0;
}
