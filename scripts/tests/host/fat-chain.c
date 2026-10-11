/*
 * Host check for scripts/tests/fat-chain.sh: files of several clusters read through the real
 * kernel/fs/file.c, pparser.c, fat/fat16.c and disk/streamer.c, built with the host C compiler, from
 * a copy of the RorthOX image (fat-host.c is the disk). Each file is read with one
 * fread(filesize, 1) from position 0 and compared byte for byte with its source. Every position
 * fat16.c seeks to during the read must be either the entry of a chain cluster in the first FAT
 * (reserved sectors * sector size + 2 * cluster, from the BPB) or inside a data cluster of the
 * chain, and every link of the chain must have been looked up there. Sector reads must stay in
 * the same places.
 *
 * Usage: fat-chain <image> <8.3 name> <source file> [<8.3 name> <source file>]...
 *        (8.3 names as in the directory entry, e.g. "CHAIN   BIN")
 * Prints one line per file and "fat-chain host check: <failed> of <files> files failed".
 */
#include "host.h"
#include "kalloc.h"
#include "config.h"
#include "status.h"
#include "fs/file.h"
#include "fat-host.h"

int fread(void *ptr, uint32_t size, uint32_t nmemb, int fd);
void *malloc(unsigned long size);
void *memset(void *ptr, int c, unsigned long size);

#define MAX_CHAIN 512

static int in_chain(long cluster, const long *chain, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (chain[i] == cluster)
        {
            return 1;
        }
    }
    return 0;
}

// The link of chain[i] (i < n - 1) is looked up at its first-FAT entry.
static int is_link_entry(long pos, const long *chain, int n)
{
    for (int i = 0; i < n - 1; i++)
    {
        if (pos == fat_entry_pos(chain[i]))
        {
            return 1;
        }
    }
    return 0;
}

static int check_file(const char *name83, const char *source)
{
    char path[32] = "0:/";
    int p = 3;
    for (int i = 0; i < 8 && name83[i] != ' '; i++)
    {
        path[p++] = name83[i];
    }
    path[p++] = '.';
    for (int i = 8; i < 11 && name83[i] != ' '; i++)
    {
        path[p++] = name83[i];
    }
    path[p] = 0;

    long want_size;
    unsigned char *want = fat_host_read_file(source, &want_size);
    fat_host_setup();
    long first, size, chain[MAX_CHAIN];
    if (fat_host_root_entry(name83, &first, &size) != 0 || size != want_size)
    {
        printf("  FAIL %s: no root entry of %ld bytes\n", path, want_size);
        return 1;
    }
    int n = fat_host_chain(first, chain, MAX_CHAIN);
    if (n < 3 || n != (size + geo.cluster_bytes - 1) / geo.cluster_bytes)
    {
        printf("  FAIL %s: chain of %d clusters for %ld bytes (need 3 or more)\n", path, n, size);
        return 1;
    }
    int contiguous = 1;
    for (int i = 1; i < n; i++)
    {
        contiguous &= chain[i] == chain[i - 1] + 1;
    }

    int fd = fopen(path, "r");
    struct file_stat stat;
    if (fd <= 0 || fstat(fd, &stat) != 0 || (long)stat.filesize != size)
    {
        printf("  FAIL %s: did not open with its size (fd %d)\n", path, fd);
        return 1;
    }
    unsigned char *data = malloc(size + 16);
    memset(data, 0, size + 16);
    fat_host_clear_logs();
    int res = fread(data, size, 1, fd);
    int failed = 0;

    // Where fat16.c went.
    long fat_start = geo.reserved_sectors * geo.sector_size;
    long fat_end = fat_start + geo.sectors_per_fat * geo.sector_size;
    for (int i = 0; i < seek_count && !failed; i++)
    {
        long pos = seek_log[i];
        if (is_link_entry(pos, chain, n) || in_chain(cluster_of_pos(pos), chain, n))
        {
            continue;
        }
        printf("  FAIL %s: seek to byte %ld, neither a link entry (%ld + 2 * cluster) nor chain data\n",
               path, pos, fat_start);
        failed = 1;
    }
    for (int i = 0; i < n - 1 && !failed; i++)
    {
        int seen = 0;
        for (int k = 0; k < seek_count; k++)
        {
            seen |= seek_log[k] == fat_entry_pos(chain[i]);
        }
        if (!seen)
        {
            printf("  FAIL %s: link of cluster %ld never looked up at byte %ld\n", path, chain[i], fat_entry_pos(chain[i]));
            failed = 1;
        }
    }
    for (int i = 0; i < read_count && !failed; i++)
    {
        long pos = read_log[i] * geo.sector_size;
        int ok = 0;
        if (pos >= fat_start && pos < fat_end)
        {
            for (int k = 0; k < n - 1; k++)
            {
                ok |= fat_entry_pos(chain[k]) / geo.sector_size == read_log[i];
            }
        }
        else
        {
            ok = in_chain(cluster_of_pos(pos), chain, n);
        }
        if (!ok)
        {
            printf("  FAIL %s: read sector %ld, outside the chain's FAT sectors and data\n", path, read_log[i]);
            failed = 1;
        }
    }
    if (!failed && res != 1)
    {
        printf("  FAIL %s: fread returned %d, expected 1\n", path, res);
        failed = 1;
    }
    if (!failed)
    {
        for (long i = 0; i < size; i++)
        {
            if (data[i] != want[i])
            {
                printf("  FAIL %s: byte %ld is %02x, expected %02x\n", path, i, data[i], want[i]);
                failed = 1;
                break;
            }
        }
    }
    fclose(fd);
    if (!failed)
    {
        int lookups = 0;
        for (int k = 0; k < seek_count; k++)
        {
            lookups += seek_log[k] >= fat_start && seek_log[k] < fat_end;
        }
        printf("  ok   %s: %ld bytes, %d %s clusters, %d FAT lookups, every byte read back\n",
               path, size, n, contiguous ? "contiguous" : "fragmented", lookups);
    }
    return failed;
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc % 2)
    {
        printf("usage: fat-chain <image> <8.3 name> <source file>...\n");
        return 2;
    }
    fat_host_open_image(argv[1]);
    int files = 0, failed = 0;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        fflush(0);
        pid_t pid = fork();
        if (pid == 0)
        {
            int res = check_file(argv[i], argv[i + 1]);
            fflush(0);
            _exit(res ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        files++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            if (WIFSIGNALED(status))
            {
                printf("  FAIL %s: crashed (signal %d)\n", argv[i], WTERMSIG(status));
            }
            failed++;
        }
    }
    printf("fat-chain host check: %d of %d files failed\n", failed, files);
    fflush(0);
    return failed ? 1 : 0;
}
