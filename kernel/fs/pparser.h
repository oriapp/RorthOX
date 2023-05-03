#ifndef PATHPARSER_H
#define PATHPARSER_H

#ifdef __cplusplus
extern "C" {
#endif

struct path_root
{
    int drive_no;
    struct path_part *first;
};

struct path_part
{
    const char *part;
    struct path_part *next;
};

struct path_root *pathparser_parse(const char *path, const char *current_directory_path);
void pathparser_free(struct path_root *root);

// 0:/text.txt
// 0:/test/text.txt

#ifdef __cplusplus
}
#endif

#endif // !PATHPARSER_H