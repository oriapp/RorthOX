#include "string.h"
#include <stddef.h>
#include <limits.h>

int vsnprintf(char *str, size_t size, const char *format, va_list args);
int vsprintf(char *str, const char *format, va_list args);

char tolower(char s1)
{
    if (s1 >= 65 && s1 <= 90)
    {
        s1 += 32;
    }

    return s1;
}

int strlen(const char *ptr)
{
    int i = 0;
    while (*ptr != 0)
    {
        i++;
        ptr += 1;
    }

    return i;
}

int strnlen(const char *ptr, int max)
{
    int i = 0;
    for (i = 0; i < max; i++)
    {
        if (ptr[i] == 0)
            break;
    }

    return i;
}

int strnlen_terminator(const char *str, int max, char terminator)
{
    int i;
    for (i = 0; i < max; i++)
    {
        if (str[i] == '\0' || str[i] == terminator)
            break;
    }

    return i;
}

int istrncmp(const char *s1, const char *s2, int n)
{
    unsigned char u1, u2;
    while(n-- > 0)
    {
        u1 = (unsigned char) *s1++;
        u2 = (unsigned char) *s2++;
        if (u1 != u2 && tolower(u1) != tolower(u2))
            return u1 - u2;
        if (u1 == '\0')
            return 0;
    }

    return 0;
}

int strncmp(const char *str1, const char *str2, int n)
{
    unsigned char u1, u2;
    
    while(n-- > 0)
    {
        u1 = (unsigned char) *str1++;
        u2 = (unsigned char) *str2++;
        if (u1 != u2)
            return u1 - u2;
        if (u1 == '\0')
            return 0;
    }

    return 0;
}

char *strcpy(char *dest, const char *src)
{
    char *res = dest;
    while(*src != 0)
    {
        *dest = *src;
        src += 1;
        dest += 1;
    }

    *dest = 0x00;

    return res;
}

char *strncpy(char *dest, const char *src, int count)
{
    int i = 0;
    for (i = 0; i < count-1; i++)
    {
        if (src[i] == 0)
            break;
        
        dest[i] = src[i];
    }

    dest[i] = 0x00;
    return dest;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);

    while (*haystack != '\0') {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return (char *) haystack;
        }
        haystack++;
    }

    return NULL;
}

// char *strrev(char *str) {
//     int len = strlen(str);
//     for (int i = 0; i < len / 2; i++) {
//         char temp = str[i];
//         str[i] = str[len-i-1];
//         str[len-i-1] = temp;
//     }
//     return str;
// }

char *strchr(const char *str, int c)
{
    while (*str != '\0') {
        if (*str == (char) c) {
            return (char *) str;
        }
        str++;
    }
    return NULL;
}

char *strcat(char *dest, const char *src)
{
    char *res = dest;
    while (*dest != 0)
    {
        dest += 1;
    }

    while (*src != 0)
    {
        *dest = *src;
        src += 1;
        dest += 1;
    }

    *dest = 0x00;

    return res;
}

bool isdigit(char c)
{
    return c >= 48 && c <= 57;
}

int tonumericdigit(char c)
{
    return c - 48;
}

/// @brief Integer to ASCII
/// @param i 
/// @return char res
char *itoa(int i)
{
    static char text[12];
    int loc = 11;
    text[11] = 0;
    char neg = 1;
    if (i >= 0)
    {
        neg = 0;
        i = -i;
    }

    while (i)
    {
        text[--loc] = '0' - (i % 10);
        i /= 10;
    }

    if (loc == 11)
    {
        text[--loc] = '0';
    }

    if (neg)
    {
        text[--loc] = '-';
    }

    return &text[loc];
}


/// @brief Convers an integer to its hexadecimal representation in ASCII
/// @param value The integer to be converted
/// @param str The output string containing the hexadecimal representation of 'vale'
/// @param base The base of the numebr system to use for the conversion (should be 16 for hexadecimal)
/// @return A pointer to the output string 'str'
char *itoa_hex(int value, char *str, int base)
{
    char *p = str;
    char *p1, *p2;
    int tmp_value;

    if (value < 0)
    {
        *p++ = '-';
        value = -value;
    }

    tmp_value = value;

    do {
        int remainder = tmp_value % base;
        *p++ = (remainder < 10) ? remainder + '0' : remainder + 'A' - 10;
    } while (tmp_value /= base);

    *p = 0;
    p1 = str;
    p2 = p - 1;
    while (p1 < p2)
    {
        char tmp = *p1;
        *p1 = *p2;
        *p2 = tmp;
        p1++;
        p2++;
    }

    return str;
}

/// @brief ASCII to Integer
/// @param str 
/// @return int res
int atoi(char* str)
{
    // Initialize result
    int res = 0;

    for (int i = 0; str[i] != '\0'; ++i)
        res = res * 10 + str[i] - '0';
  
    // return result.
    return res;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    int count;
    va_list args;
    va_start(args, format);
    count = vsnprintf(str, size, format, args);
    va_end(args);
    return count;
}

int sprintf(char *str, const char *format, ...) {
    int count;
    va_list args;
    va_start(args, format);
    count = vsprintf(str, format, args);
    va_end(args);
    return count;
}


int vsnprintf(char *str, size_t size, const char *format, va_list args) {
    int count;
    if (size == 0) {
        return 0;
    }
    count = vsprintf(str, format, args);
    if (count < size) {
        return count;
    }
    str[size - 1] = '\0';
    return size - 1;
}

int vsprintf(char *str, const char *format, va_list args) {
    int count;
    count = vsnprintf(str, INT_MAX, format, args);
    return count;
}