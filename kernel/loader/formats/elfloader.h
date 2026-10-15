#ifndef ELFLOADER_H
#define ELFLOADER_H
#include <stdint.h>
#include <stddef.h>

#include "elf.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct elf_file
{
    char filename[RORTHOX_MAX_PATH];

    uint32_t in_memory_size;
    void **segment_memory;
    unsigned int segment_count;

    // The physical memory address that this elf file loaded at
    void *elf_memory;
    
    // The virtual base address of this binary
    void *virtual_base_address;

    // The ending virtual address
    void *virtual_end_address;

    // Physical start of the lowest virtual segment
    void *physical_base_address;

    // Physical end of the highest virtual segment (not contiguous backing)
    void *physical_end_address;
};

void *elf_virtual_base(struct elf_file *file);
void *elf_virtual_end(struct elf_file *file);
void *elf_phys_base(struct elf_file *file);
void *elf_phys_end(struct elf_file *file);
void *elf_memory(struct elf_file *file);
struct elf_header *elf_header(struct elf_file *file);
struct elf32_shdr *elf_sheader(struct elf_header *header);
struct elf32_phdr *elf_pheader(struct elf_header *header);
struct elf32_phdr *elf_program_header(struct elf_header *header, int index);
struct elf32_shdr *elf_section(struct elf_header *header, int index);
void *elf_phdr_phys_address(struct elf_file *file, struct elf32_phdr *phdr);
int elf_load(const char *filename, struct elf_file **file_out);
void elf_close(struct elf_file *file);

#ifdef __cplusplus
}
#endif

#endif // !ELFLOADER_H