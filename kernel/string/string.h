#ifndef STRING_H
#define STRING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int strlen(const char *ptr);
int strnlen(const char *ptr, int max);
bool isdigit(char c);
int tonumericdigit(char c);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, int count);
char *strstr(const char *haystack, const char *needle);
// char *strrev(char *str);
char *strchr(const char *str, int c);
char *strcat(char *dest, const char *src);
int strncmp(const char *str1, const char *str2, int n);
int istrncmp(const char *s1, const char *s2, int n);
int strnlen_terminator(const char *str, int max, char terminator);
char tolower(char s1);
char *itoa(int i);
char *itoa_hex(int value, char *str, int base);
int atoi(char* str);
int snprintf(char *str, size_t size, const char *format, ...);
int sprintf(char *str, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif // !STRING_H