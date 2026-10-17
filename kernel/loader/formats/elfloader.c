#include "elfloader.h"
#include "fs/file.h"
#include "status.h"
#include <stdbool.h>
#include "memory/memory.h"
#include "memory/heap/kheap.h"
#include "string/string.h"
#include "memory/paging/paging.h"
#include "kernel.h"
#include "config.h"

const char elf_signature[] = {0x7f, 'E', 'L', 'F'};

static bool elf_valid_signature(void *buffer)
{
    return memcmp(buffer, (void*) elf_signature, sizeof(elf_signature)) == 0;
}

void *elf_memory(struct elf_file *file)
{
    return file->elf_memory;
}

struct elf_header *elf_header(struct elf_file *file)
{
    return file->elf_memory;
}

struct elf32_shdr *elf_sheader(struct elf_header *header)
{
    return (struct elf32_shdr*)((int)header+header->e_shoff);
}

struct elf32_phdr *elf_pheader(struct elf_header *header)
{
    if (header->e_phoff == 0)
    {
        return 0;
    }

    return (struct elf32_phdr*)((int)header+header->e_phoff);
}

struct elf32_phdr *elf_program_header(struct elf_header *header, int index)
{
    return &elf_pheader(header)[index];
}

struct elf32_shdr *elf_section(struct elf_header *header, int index)
{
    return &elf_sheader(header)[index];
}

void *elf_phdr_phys_address(struct elf_file *file, struct elf32_phdr *phdr)
{
    unsigned int index = phdr - elf_pheader(elf_header(file));
    return file->segment_memory[index] + (phdr->p_vaddr & (PAGING_PAGE_SIZE - 1));
}

char *elf_str_table(struct elf_header *header)
{
    return (char*) header +elf_section(header, header->e_shstrndx)->sh_offset;
}

void *elf_virtual_base(struct elf_file *file)
{
    return file->virtual_base_address;
}

void *elf_virtual_end(struct elf_file *file)
{
    return file->virtual_end_address;
}

void *elf_phys_base(struct elf_file *file)
{
    return file->physical_base_address;
}

void *elf_phys_end(struct elf_file *file)
{
    return file->physical_end_address;
}

int elf_validate_loaded(struct elf_file *file)
{
    if (file->in_memory_size < sizeof(elf_signature) || !elf_valid_signature(file->elf_memory))
    {
        return -EINFORMAT;
    }
    if (file->in_memory_size < sizeof(struct elf_header))
    {
        return -EINVARG;
    }

    struct elf_header *header = elf_header(file);
    if (header->e_ident[EI_CLASS] != ELFCLASS32 || header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_ident[EI_VERSION] != EV_CURRENT || header->e_version != EV_CURRENT ||
        header->e_machine != EM_386 || header->e_type != ET_EXEC ||
        header->e_ehsize != sizeof(struct elf_header) || header->e_phentsize != sizeof(struct elf32_phdr))
    {
        return -EINVARG;
    }
    if (!header->e_phnum || header->e_phoff < sizeof(struct elf_header) ||
        header->e_phoff > file->in_memory_size ||
        header->e_phnum > (file->in_memory_size - header->e_phoff) / sizeof(struct elf32_phdr))
    {
        return -EINVARG;
    }

    uint32_t previous_end = RORTHOX_PROGRAM_VIRTUAL_ADDRESS;
    bool entry_found = false;
    for (int i = 0; i < header->e_phnum; i++)
    {
        struct elf32_phdr *phdr = elf_program_header(header, i);
        if (phdr->p_type == PT_INTERP || phdr->p_type == PT_DYNAMIC)
        {
            return -EINVARG;
        }
        if (phdr->p_type != PT_LOAD)
        {
            continue;
        }
        if (phdr->p_offset > file->in_memory_size ||
            phdr->p_filesz > file->in_memory_size - phdr->p_offset || phdr->p_filesz > phdr->p_memsz ||
            (phdr->p_vaddr & (PAGING_PAGE_SIZE - 1)) != (phdr->p_offset & (PAGING_PAGE_SIZE - 1)))
        {
            return -EINVARG;
        }
        if (phdr->p_align > 1 && ((phdr->p_align & (phdr->p_align - 1)) ||
            (phdr->p_vaddr % phdr->p_align) != (phdr->p_offset % phdr->p_align)))
        {
            return -EINVARG;
        }
        if (!phdr->p_memsz)
        {
            continue;
        }
        if (phdr->p_vaddr < RORTHOX_PROGRAM_VIRTUAL_ADDRESS ||
            phdr->p_vaddr >= RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT ||
            phdr->p_memsz > RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT - phdr->p_vaddr)
        {
            return -EINVARG;
        }

        uint32_t page_start = phdr->p_vaddr & ~(PAGING_PAGE_SIZE - 1);
        uint32_t end = phdr->p_vaddr + phdr->p_memsz;
        uint32_t page_end = (end + PAGING_PAGE_SIZE - 1) & ~(PAGING_PAGE_SIZE - 1);
        if (page_start < previous_end || page_end > RORTHOX_PROGRAM_VIRTUAL_ADDRESS_LIMIT)
        {
            return -EINVARG;
        }
        previous_end = page_end;
        if ((phdr->p_flags & PF_X) && header->e_entry >= phdr->p_vaddr && header->e_entry < end)
        {
            entry_found = true;
        }
    }

    return entry_found ? RORTHOX_ALL_OK : -EINVARG;
}

int elf_process_phdr_pt_load(struct elf_file *elf_file, struct elf32_phdr *phdr)
{
    if (!phdr->p_memsz)
    {
        return 0;
    }

    unsigned int index = phdr - elf_pheader(elf_header(elf_file));
    uint32_t page_offset = phdr->p_vaddr & (PAGING_PAGE_SIZE - 1);
    uint32_t size = (page_offset + phdr->p_memsz + PAGING_PAGE_SIZE - 1) & ~(PAGING_PAGE_SIZE - 1);
    void *memory = kzalloc(size);
    if (!memory)
    {
        return -ENOMEM;
    }
    elf_file->segment_memory[index] = memory;
    if (phdr->p_filesz)
    {
        memcpy(memory + page_offset, elf_memory(elf_file) + phdr->p_offset, phdr->p_filesz);
    }

    if (elf_file->virtual_base_address >= (void*) phdr->p_vaddr || elf_file->virtual_base_address == 0x00)
    {
        elf_file->virtual_base_address = (void*) phdr->p_vaddr;

        elf_file->physical_base_address = memory + page_offset;
    }

    unsigned int end_virtual_address = phdr->p_vaddr + phdr->p_memsz;
    if (elf_file->virtual_end_address <= (void*)(end_virtual_address) || elf_file->virtual_end_address == 0x00)
    {
        elf_file->virtual_end_address = (void*) end_virtual_address;
        elf_file->physical_end_address = memory + page_offset + phdr->p_memsz;
    }
    return 0;
}

int elf_process_pheader(struct elf_file *elf_file, struct elf32_phdr *phdr)
{
    int res = 0;
    switch (phdr->p_type)
    {
    case PT_LOAD:
        res = elf_process_phdr_pt_load(elf_file, phdr);
        break;
    }

    return res;
}

int elf_process_pheaders(struct elf_file *elf_file)
{
    int res = 0;
    struct elf_header *header = elf_header(elf_file);
    for (int i = 0; i < header->e_phnum; i++)
    {
        struct elf32_phdr *phdr = elf_program_header(header, i);
        res = elf_process_pheader(elf_file, phdr);
        if (res < 0)
        {
            break;
        }
    }

    return res;
}

int elf_process_loaded(struct elf_file *elf_file)
{
    int res = 0;
    struct elf_header *header = elf_header(elf_file);
    res = elf_validate_loaded(elf_file);
    if (res < 0)
    {
        goto out;
    }

    elf_file->segment_memory = kzalloc(header->e_phnum * sizeof(void*));
    if (!elf_file->segment_memory)
    {
        res = -ENOMEM;
        goto out;
    }
    elf_file->segment_count = header->e_phnum;
    res = elf_process_pheaders(elf_file);
    if (res < 0)
    {
        goto out;
    }

out:
    return res;
}

int elf_load(const char *filename, struct elf_file **file_out)
{
    int fd = 0;
    int res = 0;
    struct elf_file *elf_file = kzalloc(sizeof(struct elf_file));
    if (!elf_file)
    {
        res = -ENOMEM;
        goto out;
    }

    res = fopen_status(filename, "r");
    if (res <= 0)
    {
        if (res != -ENOENT)
        {
            res = -EIO;
        }
        goto out;
    }

    fd = res;
    struct file_stat stat;
    res = fstat(fd, &stat);
    if (res < 0)
    {
        goto out;
    }

    // Classify before allocating the payload: raw files have their own size bound.
    char signature[sizeof(elf_signature)];
    if (stat.filesize < sizeof(signature))
    {
        res = -EINFORMAT;
        goto out;
    }

    res = fread(signature, sizeof(signature), 1, fd);
    if (res < 0)
    {
        goto out;
    }
    if (res != 1)
    {
        res = -EIO;
        goto out;
    }
    if (!elf_valid_signature(signature))
    {
        res = -EINFORMAT;
        goto out;
    }

    elf_file->in_memory_size = stat.filesize;
    elf_file->elf_memory = kzalloc(stat.filesize);
    if (!elf_file->elf_memory)
    {
        res = -ENOMEM;
        goto out;
    }

    memcpy(elf_file->elf_memory, signature, sizeof(signature));
    if (stat.filesize > sizeof(signature))
    {
        res = fread(elf_file->elf_memory + sizeof(signature), stat.filesize - sizeof(signature), 1, fd);
        if (res < 0)
        {
            goto out;
        }
        if (res != 1)
        {
            res = -EIO;
            goto out;
        }
    }

    res = elf_process_loaded(elf_file);
    if (res < 0)
    {
        goto out;
    }

    *file_out = elf_file;

out:
    fclose(fd);
    if (res < 0)
    {
        elf_close(elf_file);
    }
    return res;
}

void elf_close(struct elf_file *file)
{
    if (!file)
    {
        return;
    }

    for (unsigned int i = 0; i < file->segment_count; i++)
    {
        kfree(file->segment_memory[i]);
    }
    kfree(file->segment_memory);
    kfree(file->elf_memory);
    kfree(file);
}
