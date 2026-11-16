#include "vfat.h"
#include "memory/memory.h"
#include "string/string.h"

uint8_t vfat_checksum(const uint8_t alias[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
    {
        sum = ((sum & 1) << 7) + (sum >> 1) + alias[i];
    }
    return sum;
}

int vfat_name_valid(const char *name)
{
    if (!name)
    {
        return 0;
    }
    int len = strnlen(name, 256);
    if (!len || len > 255 || name[0] == ' ' || name[len - 1] == ' ' || name[len - 1] == '.')
    {
        return 0;
    }
    for (int i = 0; i < len; i++)
    {
        unsigned char c = name[i];
        if (c < 0x20 || c > 0x7E)
        {
            return 0;
        }
        const char *bad = "\"*/:<>?\\|";
        for (int j = 0; bad[j]; j++)
        {
            if (c == bad[j])
            {
                return 0;
            }
        }
    }
    return 1;
}

void vfat_lfn_reset(struct vfat_lfn *state)
{
    memset(state, 0, sizeof(*state));
}

// FAT long entries arrive in reverse order; only the first physical fragment may end the name.
void vfat_lfn_feed(struct vfat_lfn *state, const uint8_t entry[32])
{
    int ordinal = entry[0] & 0x3F;
    int last = entry[0] & 0x40;
    int started = state->started;
    if (last && !started)
    {
        state->next = ordinal;
        state->checksum = entry[13];
        state->valid = 1;
    }
    state->started = 1;
    if (!state->valid || (last && started) || (entry[0] & 0x80) || ordinal < 1 || ordinal > 20 ||
        ordinal != state->next || entry[11] != 0x0F || entry[12] || entry[26] || entry[27] ||
        entry[13] != state->checksum)
    {
        state->valid = 0;
        return;
    }
    static const uint8_t offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int ended = 0;
    for (int i = 0; i < 13; i++)
    {
        int offset = offsets[i];
        unsigned int c = entry[offset] | (entry[offset + 1] << 8);
        int index = (ordinal - 1) * 13 + i;
        if ((ended && c != 0xFFFF) || (!ended && !c && (!last || !i)) ||
            (!ended && c && (c < 0x20 || c > 0x7E || index >= 255)))
        {
            state->valid = 0;
            return;
        }
        if (!c)
        {
            ended = 1;
            state->length = index;
        }
        else if (!ended)
        {
            state->name[index] = c;
            if (last)
            {
                state->length = index + 1;
            }
        }
    }
    state->next--;
}

int vfat_lfn_finish(struct vfat_lfn *state, const uint8_t alias[11])
{
    if (!state->valid || state->next || state->checksum != vfat_checksum(alias))
    {
        return 0;
    }
    state->name[state->length] = 0;
    return vfat_name_valid(state->name);
}

void vfat_short_name(const uint8_t entry[32], char out[13])
{
    int n = 0;
    for (int field = 0; field < 2; field++)
    {
        int start = field ? 8 : 0;
        int end = field ? 11 : 8;
        while (end > start && entry[end - 1] == ' ')
        {
            end--;
        }
        if (field && end > start)
        {
            out[n++] = '.';
        }
        for (int i = start; i < end; i++)
        {
            unsigned char c = entry[i];
            if (c < 0x20 || c > 0x7E)
            {
                c = '?';
            }
            if ((entry[12] & (field ? 0x10 : 0x08)) && c >= 'A' && c <= 'Z')
            {
                c += 'a' - 'A';
            }
            out[n++] = c;
        }
    }
    out[n] = 0;
}

int vfat_short_match(const uint8_t entry[32], const char *name)
{
    // Paths are printable ASCII; raw OEM/control bytes cannot match, even when displayed as '?'.
    for (int i = 0; i < 11; i++)
    {
        if (entry[i] < 0x20 || entry[i] > 0x7E)
        {
            return 0;
        }
    }
    char alias[13];
    vfat_short_name(entry, alias);
    return istrncmp(alias, name, sizeof(alias)) == 0;
}
