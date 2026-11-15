#pragma once
#define CURRENT_YEAR 2023 // Change this each year!
#include "io/io.h"
#include "include/kernel/tty.h"
#include "string/string.h"
#include "include/kernel/sys.h"
#include "memory/heap/kheap.h"
#include <stdint.h>

int century_register = 0x00; // Set by ACPI table parsing code if possible

unsigned char second;
unsigned char minute;
unsigned char hour;
unsigned char day;
unsigned char month;
unsigned int year;

enum
{
    cmos_address = 0x70,
    cmos_data = 0x71
};

int get_update_in_progress_flag()
{
    outb((unsigned short)cmos_address, 0x0A);
    return (insb((unsigned short)cmos_data) & 0x80);
}

unsigned char get_RTC_register(int reg)
{
    outb((unsigned short)cmos_address, reg);
    return insb((unsigned short)cmos_data);
}

int timezone_offset = 180; // -3 hours (in minutes)

void read_rtc()
{
    unsigned char century = 0;
    unsigned char last_second;
    unsigned char last_minute;
    unsigned char last_hour;
    unsigned char last_day;
    unsigned char last_month;
    unsigned char last_year;
    unsigned char last_century;
    unsigned char registerB;

    // Note: This uses the "read registers until you get the same values twice in a row" technique
    //       to avoid getting dodgy/inconsistent values due to RTC updates

    while (get_update_in_progress_flag())
        ; // Make sure an update isn't in progress
    second = get_RTC_register(0x00);
    minute = get_RTC_register(0x02);
    hour = get_RTC_register(0x04);
    day = get_RTC_register(0x07);
    month = get_RTC_register(0x08);
    year = get_RTC_register(0x09);
    if (century_register != 0)
    {
        century = get_RTC_register(century_register);
    }

    do
    {
        last_second = second;
        last_minute = minute;
        last_hour = hour;
        last_day = day;
        last_month = month;
        last_year = year;
        last_century = century;

        while (get_update_in_progress_flag())
            ; // Make sure an update isn't in progress
        second = get_RTC_register(0x00);
        minute = get_RTC_register(0x02);
        hour = get_RTC_register(0x04);
        day = get_RTC_register(0x07);
        month = get_RTC_register(0x08);
        year = get_RTC_register(0x09);
        if (century_register != 0)
        {
            century = get_RTC_register(century_register);
        }
    } while ((last_second != second) || (last_minute != minute) || (last_hour != hour) ||
             (last_day != day) || (last_month != month) || (last_year != year) ||
             (last_century != century));

    registerB = get_RTC_register(0x0B);

    // Convert BCD to binary values if necessary

    if (!(registerB & 0x04))
    {
        second = (second & 0x0F) + ((second / 16) * 10);
        minute = (minute & 0x0F) + ((minute / 16) * 10);
        hour = ((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80);
        day = (day & 0x0F) + ((day / 16) * 10);
        month = (month & 0x0F) + ((month / 16) * 10);
        year = (year & 0x0F) + ((year / 16) * 10);
        if (century_register != 0)
        {
            century = (century & 0x0F) + ((century / 16) * 10);
        }
    }

    // Convert 12 hour clock to 24 hour clock if necessary

    if (!(registerB & 0x02) && (hour & 0x80))
    {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    // Calculate the full (4-digit) year

    if (century_register != 0)
    {
        year += century * 100;
    }
    else
    {
        year += (CURRENT_YEAR / 100) * 100;
        if (year < CURRENT_YEAR)
            year += 100;
    }
}

static int rtc_format_token(char *buffer, size_t capacity, char token)
{
    int value;
    switch (token)
    {
    case 'h':
    case 'H':
        value = hour;
        break;
    case 'm':
    case 'M':
        value = minute;
        break;
    case 's':
    case 'S':
        value = second;
        break;
    default:
        return snprintf(buffer, capacity, "%c", token);
    }

    if (token == 'h' || token == 'm' || token == 's')
        return snprintf(buffer, capacity, "%02d", value);
    return snprintf(buffer, capacity, "%s%d", value < 10 ? " " : "", value);
}

char *getformattedtime(const char *format)
{
    if (!format)
        return NULL;

    size_t capacity = 1;
    for (const char *p = format; *p; p++)
    {
        char token[4]; // An unsigned RTC byte needs at most three digits.
        int length = rtc_format_token(token, sizeof(token), *p);
        if (length < 0 || (size_t)length >= sizeof(token) ||
            (size_t)length > SIZE_MAX - capacity)
            return NULL;
        capacity += length;
    }

    char *result = kmalloc(capacity);
    if (!result)
        return NULL;
    result[0] = '\0';

    size_t used = 0;
    for (const char *p = format; *p; p++)
    {
        int length = rtc_format_token(result + used, capacity - used, *p);
        if (length < 0 || (size_t)length >= capacity - used)
        {
            kfree(result);
            return NULL;
        }
        used += length;
    }
    return result;
}

char *gettime()
{
    size_t capacity = sizeof("255:255:255");
    char *result = kmalloc(capacity);
    if (!result)
        return NULL;
    int length = snprintf(result, capacity, "%d:%d:%d", hour, minute, second);
    if (length < 0 || (size_t)length >= capacity)
    {
        kfree(result);
        return NULL;
    }
    return result;
}

void printtime()
{
    char time[sizeof("255:255:255\n")];
    read_rtc();

    int length = snprintf(time, sizeof(time), "%02d:%02d:%02d\n", hour, minute, second);
    if (length < 0 || (size_t)length >= sizeof(time))
        return;
    printc("System Time: ", VGA_COLOR_LIGHT_GREEN);
    print(time);
}