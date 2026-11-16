#ifndef VFAT_H
#define VFAT_H

#include <stdint.h>

struct vfat_lfn
{
    char name[256];
    int length;
    uint8_t next;
    uint8_t checksum;
    uint8_t valid;
    uint8_t started;
};

uint8_t vfat_checksum(const uint8_t alias[11]);
int vfat_name_valid(const char *name);
void vfat_lfn_reset(struct vfat_lfn *state);
void vfat_lfn_feed(struct vfat_lfn *state, const uint8_t entry[32]);
int vfat_lfn_finish(struct vfat_lfn *state, const uint8_t alias[11]);
// Display only: nonprintable/OEM bytes (including NUL and first-byte 0x05) become '?'.
// NTRes case bits affect ASCII letters only. Matching never uses these substitutions.
void vfat_short_name(const uint8_t entry[32], char out[13]);
int vfat_short_match(const uint8_t entry[32], const char *name);

#endif
