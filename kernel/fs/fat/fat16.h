#ifndef FAT16_H
#define FAT16_H
#include "fs/file.h"

#ifdef __cplusplus
extern "C" {
#endif

struct filesystem * fat16_init();

#ifdef __cplusplus
}
#endif

#endif // !FAT16_H