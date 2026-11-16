/*
 * Host check for scripts/tests/fat-vfat-read.sh, the parts the public calls can't show: the
 * VFAT codec (kernel/fs/fat/vfat.c) and the directory record scanner of fat16.c (included here
 * to reach its static functions). Built with AddressSanitizer and UndefinedBehaviorSanitizer.
 *
 *   checksum   known answer, and the checksum of each alias equals the one mtools stored in
 *              every long-name entry of its set
 *   decode     the sets mtools wrote (13, 26 and 255 characters, and the two that cross the
 *              fragmented chain of 'Split directory') decode to the name mtools was given
 *   damaged    every damaged copy of the 255-character set decodes to nothing; with 256 or 260
 *              characters the decoder already refuses the set while it is fed, before the
 *              alias (a name longer than 255 is never built); an entry with an ordinal outside
 *              1..20 writes nothing (its state sits in an exact-size allocation, so a write
 *              past it is an AddressSanitizer report)
 *   names      vfat_name_valid: the long-name rules of the extended paths
 *   short      how short names are shown (every byte outside 0x20-0x7E is '?', the NTRes
 *              lowercase bits change ASCII letters only) and matched (raw bytes: an odd alias
 *              matches no path, not even its '?' form); the output is exactly 13 bytes
 *   listing    fat16_scan_record over a directory gives exactly the records of
 *              listing-<n>.txt in order (long name or shown alias, shown alias, long name valid),
 *              skips labels and dot entries, makes no allocation and ends with 0, also on
 *              odd.img; on the images whose 'Split directory' record has a bad first cluster
 *              the root scan stops there with -EIO
 *   lookup     fat16_lookup_extended through a long parent makes no allocation and reports
 *              the parent's cluster
 *
 * Usage: fat-vfat-codec <fixture dir>. Prints one line per failed check, per part a count,
 * then "fat-vfat-codec host check: <failed> of <parts> parts failed".
 */
#include "host.h"
#include "kalloc.h"
#include "fat-image.h"
#include "fs/fat/fat16.c"

// kernel/string/string.c has its own snprintf, so format with the C library's.
int __snprintf_chk(char *str, unsigned long size, int flag, unsigned long slen, const char *format, ...);
#define host_snprintf(str, size, ...) __snprintf_chk((str), (size), 0, (size), __VA_ARGS__)
unsigned int alarm(unsigned int seconds);
void *malloc(unsigned long size);
void free(void *ptr);

static int checks, bad;
static const char *dir;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("    line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); bad++; } } while (0)

static long read_host_file(const char *path, void *buf, long max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }
    long n = 0, r;
    while (n < max && (r = read(fd, (char *)buf + n, max - n)) > 0)
    {
        n += r;
    }
    close(fd);
    return n;
}

// A set file: its long-name entries then the alias entry. Returns the number of entries.
static int load_set(const char *tag, uint8_t *set, char *name)
{
    char path[512];
    host_snprintf(path, sizeof(path), "%s/sets/%s.bin", dir, tag);
    long n = read_host_file(path, set, 22 * 32);
    if (n <= 0 || n % 32)
    {
        printf("    cannot read %s\n", path);
        bad++;
        return 0;
    }
    if (name)
    {
        host_snprintf(path, sizeof(path), "%s/sets/%s.name", dir, tag);
        long len = read_host_file(path, name, 256);
        name[len < 0 ? 0 : len] = 0;
    }
    return n / 32;
}

// Feed a set's long-name entries to a fresh state, then finish with its alias.
static int decode(struct vfat_lfn *state, const uint8_t *set, int entries)
{
    vfat_lfn_reset(state);
    for (int i = 0; i < entries - 1; i++)
    {
        vfat_lfn_feed(state, set + 32 * i);
    }
    return vfat_lfn_finish(state, set + 32 * (entries - 1));
}

static const char *mtools_sets[] = {"13", "26", "255", "cross", "cross200"};

static void case_checksum(void)
{
    uint8_t alias[11];
    memcpy(alias, "FOO     TXT", 11);
    CHECK(vfat_checksum(alias) == 0x65, "checksum of FOO.TXT is %02x", vfat_checksum(alias));
    for (unsigned s = 0; s < sizeof(mtools_sets) / sizeof(mtools_sets[0]); s++)
    {
        uint8_t set[22 * 32];
        int n = load_set(mtools_sets[s], set, NULL);
        for (int i = 0; i + 1 < n; i++)
        {
            CHECK(set[32 * i + 13] == vfat_checksum(set + 32 * (n - 1)), "set %s entry %d: mtools stored %02x",
                  mtools_sets[s], i, set[32 * i + 13]);
        }
    }
}

static void case_decode(void)
{
    struct vfat_lfn *state = malloc(sizeof(*state));
    for (unsigned s = 0; s < sizeof(mtools_sets) / sizeof(mtools_sets[0]); s++)
    {
        uint8_t set[22 * 32];
        char name[257];
        int n = load_set(mtools_sets[s], set, name);
        CHECK(n == ((int)strlen(name) + 12) / 13 + 1, "set %s has %d entries", mtools_sets[s], n);
        CHECK(decode(state, set, n) == 1, "set %s does not decode", mtools_sets[s]);
        CHECK(strcmp(state->name, name) == 0, "set %s decodes to \"%s\"", mtools_sets[s], state->name);
    }
    free(state);
}

static void case_damaged(void)
{
    char list[4096], path[512];
    host_snprintf(path, sizeof(path), "%s/sets/damaged.txt", dir);
    long len = read_host_file(path, list, sizeof(list) - 1);
    CHECK(len > 0, "cannot read %s", path);
    list[len < 0 ? 0 : len] = 0;
    int count = 0;
    struct vfat_lfn *state = malloc(sizeof(*state));
    for (char *p = list; *p;)
    {
        char *tag = p;
        while (*p != '\t')
        {
            p++;
        }
        *p++ = 0;
        char *what = p;
        while (*p != '\n')
        {
            p++;
        }
        *p++ = 0;
        uint8_t set[22 * 32];
        int n = load_set(tag, set, NULL);
        CHECK(decode(state, set, n) == 0, "%s (%s) decodes to \"%s\"", tag, what, state->name);
        count++;
        if (strcmp(tag, "length256") == 0 || strcmp(tag, "length260") == 0)
        {
            vfat_lfn_reset(state);
            for (int i = 0; i < n - 1; i++)
            {
                vfat_lfn_feed(state, set + 32 * i);
            }
            CHECK(!state->valid, "%s: still valid after the long-name entries", tag);
            CHECK(state->length <= 255, "%s: length %d", tag, state->length);
        }
    }
    CHECK(count >= 30, "only %d damaged sets", count);
    // Ordinals outside 1..20 (with and without LAST) write nothing into the state.
    for (int ordinal = 0; ordinal < 0x100; ordinal++)
    {
        if ((ordinal & 0x3F) >= 1 && (ordinal & 0x3F) <= 20 && !(ordinal & 0x80))
        {
            continue;
        }
        struct vfat_lfn *exact = malloc(sizeof(*exact));
        uint8_t entry[32];
        memset(entry, 'x', sizeof(entry));
        entry[0] = ordinal;
        entry[11] = 0x0F;
        entry[12] = entry[26] = entry[27] = 0;
        vfat_lfn_reset(exact);
        vfat_lfn_feed(exact, entry);
        int written = 0;
        for (int i = 0; i < 256; i++)
        {
            written |= exact->name[i];
        }
        CHECK(!written && !exact->valid, "ordinal byte %02x: name written or state valid", ordinal);
        free(exact);
    }
    free(state);
}

static void case_names(void)
{
    char long_ok[256], long_bad[257];
    memset(long_ok, 'a', 255);
    long_ok[255] = 0;
    memset(long_bad, 'a', 256);
    long_bad[256] = 0;
    const char *good[] = {"a", "a b", "x.y.z", ".hidden", "+,;=[]", "a~1", "A.B C", long_ok};
    const char *refused[] = {"", ".", "..", " a", "a ", "a.", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b",
                             "a<b", "a>b", "a|b", "a\x01", "a\x1f", "a\x7f", "a\x80", "\xe9", long_bad};
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
    {
        CHECK(vfat_name_valid(good[i]) == 1, "\"%s\" refused", good[i]);
    }
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    {
        CHECK(vfat_name_valid(refused[i]) == 0, "\"%s\" accepted", refused[i]);
    }
    CHECK(vfat_name_valid(NULL) == 0, "NULL accepted");
}

static void case_short(void)
{
    static const struct { const char *raw; int ntres; const char *shown; } shown[] = {
        {"FOO     TXT", 0x00, "FOO.TXT"},
        {"FOO     TXT", 0x08, "foo.TXT"},
        {"FOO     TXT", 0x10, "FOO.txt"},
        {"FOO     TXT", 0x18, "foo.txt"},
        {"README     ", 0x00, "README"},
        {"A       B  ", 0x00, "A.B"},
        {"\x05OO     TXT", 0x00, "?OO.TXT"},        // 0x05 stands for OEM 0xE5
        {"A\x01" "B     TXT", 0x00, "A?B.TXT"},
        {"\x80" "BC     TXT", 0x08, "?bc.TXT"},
        {"NUL\x00    TXT", 0x00, "NUL?.TXT"},
        {"\x7f" "EL     TXT", 0x18, "?el.txt"},
        {"\xc1\xc2      \xc3  ", 0x18, "??.?"},          // letters + 0x80 are not ASCII letters
        {"\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 0x18, "????????.???"},
    };
    for (unsigned i = 0; i < sizeof(shown) / sizeof(shown[0]); i++)
    {
        uint8_t entry[32];
        memset(entry, 0, sizeof(entry));
        memcpy(entry, shown[i].raw, 11);
        entry[12] = shown[i].ntres;
        char *out = malloc(13);
        memset(out, 'z', 13);
        vfat_short_name(entry, out);
        CHECK(strnlen(out, 13) < 13 && strcmp(out, shown[i].shown) == 0, "case %u shown as \"%.13s\", expected \"%s\"",
              i, out, shown[i].shown);
        free(out);
    }
    static const struct { const char *raw; int ntres; const char *name; int match; } match[] = {
        {"FOO     TXT", 0x00, "FOO.TXT", 1},
        {"FOO     TXT", 0x00, "foo.txt", 1},
        {"FOO     TXT", 0x18, "FOO.TXT", 1},
        {"README     ", 0x00, "readme", 1},
        {"README     ", 0x00, "README.", 0},
        {"FOO     TXT", 0x00, "FOO", 0},
        {"FOO     TXT", 0x00, "FOO.TXTX", 0},
        {"FOO     TXT", 0x00, "FOO .TXT", 0},
        {"\x05OO     TXT", 0x00, "?OO.TXT", 0},
        {"A\x01" "B     TXT", 0x00, "A?B.TXT", 0},
        {"NUL\x00    TXT", 0x00, "NUL?.TXT", 0},
        {"NUL\x00    TXT", 0x00, "NUL", 0},
        {"NUL\x00    TXT", 0x00, "NUL.TXT", 0},
    };
    for (unsigned i = 0; i < sizeof(match) / sizeof(match[0]); i++)
    {
        uint8_t entry[32];
        memset(entry, 0, sizeof(entry));
        memcpy(entry, match[i].raw, 11);
        entry[12] = match[i].ntres;
        CHECK(vfat_short_match(entry, match[i].name) == match[i].match, "match case %u (%s): %d", i,
              match[i].name, vfat_short_match(entry, match[i].name));
    }
}

static char got_list[64 * 1024];

// Scan one directory of <image>; compare with <dir>/listing-<tag>.txt. <end> = what the scan must end with.
static void listing(const char *image, const char *parent, const char *tag, int end)
{
    char path[512];
    host_snprintf(path, sizeof(path), "%s/%s", dir, image);
    fimg_open(path);
    CHECK(fimg_mount() == 0, "%s does not mount", image);
    struct disk *disk = disk_get(0);
    int cluster = 0;
    if (parent)
    {
        struct fat_record record;
        struct path_root *root = pathparser_parse_extended(parent, 0);
        int parent_cluster = -1;
        CHECK(!ISERR(root) && fat16_lookup_extended(disk, root->first, &parent_cluster, &record) == 0,
              "%s: no %s", image, parent);
        pathparser_free_extended(root);
        cluster = fat16_get_first_cluster(&record.entry);
    }
    int calls = kalloc_calls(), res, n = 0;
    got_list[0] = 0;
    struct fat_directory_cursor cursor;
    CHECK(fat16_directory_start(disk, cluster, &cursor) == 0, "%s: directory start", image);
    struct fat_record record;
    while ((res = fat16_scan_record(&cursor, &record)) > 0)
    {
        n += host_snprintf(got_list + n, sizeof(got_list) - n, "%s|%s|%d\n", record.name, record.short_name,
                      record.lfn_valid);
    }
    CHECK(res == end, "%s: the scan ended with %d, expected %d", image, res, end);
    CHECK(kalloc_calls() == calls, "%s: the scan made %d allocations", image, kalloc_calls() - calls);
    static char want[64 * 1024];
    host_snprintf(path, sizeof(path), "%s/listing-%s.txt", dir, tag);
    long len = read_host_file(path, want, sizeof(want) - 1);
    want[len < 0 ? 0 : len] = 0;
    if (strcmp(got_list, want) != 0)
    {
        CHECK(0, "%s: the listing differs from listing-%s.txt; got:\n%s", image, tag, got_list);
    }
}

static void case_listing(void)
{
    listing("foreign.img", "0:/Split directory", "foreign-split", 0);
    listing("odd.img", NULL, "odd", 0);
    listing("dir-cluster0.img", NULL, "before-split", -EIO);
    listing("dir-cluster1.img", NULL, "before-split", -EIO);
    listing("dir-cluster-max.img", NULL, "before-split", -EIO);
    listing("dir-cluster-ffff.img", NULL, "before-split", -EIO);
}

static void case_lookup(void)
{
    char path[512];
    host_snprintf(path, sizeof(path), "%s/foreign.img", dir);
    fimg_open(path);
    CHECK(fimg_mount() == 0, "foreign.img does not mount");
    struct path_root *root = pathparser_parse_extended("0:/split DIRECTORY/crosses A CLUSTER EDGE.TXT", 0);
    CHECK(!ISERR(root), "path refused");
    if (ISERR(root))
    {
        return;
    }
    struct fat_record record, split;
    int parent = -1, top = -1, calls = kalloc_calls();
    CHECK(fat16_lookup_extended(disk_get(0), root->first, &parent, &record) == 0, "lookup failed");
    CHECK(kalloc_calls() == calls, "the lookup made %d allocations", kalloc_calls() - calls);
    CHECK(record.lfn_valid && strcmp(record.name, "Crosses a cluster edge.txt") == 0, "found \"%s\"", record.name);
    root->first->next = NULL;
    CHECK(fat16_lookup_extended(disk_get(0), root->first, &top, &split) == 0 && top == 0, "parent lookup");
    CHECK(parent == fat16_get_first_cluster(&split.entry) && parent >= 2, "parent cluster %d", parent);
    // A directory is not a file for the extended READ open (fat16.c refuses it before the file
    // layer would, and loads nothing for it).
    root->first->next = NULL;
    int live = kalloc_live();
    struct fat_item *item = fat16_get_extended_file(disk_get(0), root->first);
    CHECK(item == ERROR(-EINVARG), "the directory gave %p", (void *)item);
    CHECK(kalloc_live() == live, "%d allocations left", kalloc_live() - live);
    root->first->next = NULL;
    pathparser_free_extended(root);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: fat-vfat-codec <fixture dir>\n");
        return 2;
    }
    dir = argv[1];
    static const struct { const char *name; void (*run)(void); } parts[] = {
        {"checksum", case_checksum}, {"decode", case_decode}, {"damaged", case_damaged},
        {"names", case_names}, {"short", case_short}, {"listing", case_listing}, {"lookup", case_lookup},
    };
    int failed = 0, count = sizeof(parts) / sizeof(parts[0]);
    for (int i = 0; i < count; i++)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(60);
            parts[i].run();
            printf("  %s: %d checks, %d failed\n", parts[i].name, checks, bad);
            fflush(0);
            _exit(bad || kalloc_bad_frees() || kalloc_poison_errors() ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            printf("  FAIL %s%s\n", parts[i].name, WIFEXITED(status) ? "" : " (killed)");
            failed++;
        }
    }
    printf("fat-vfat-codec host check: %d of %d parts failed\n", failed, count);
    fflush(0);
    return failed ? 1 : 0;
}
