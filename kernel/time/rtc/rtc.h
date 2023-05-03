#pragma once
#define CURRENT_YEAR 2023 // Change this each year!
#include "io/io.h"
#include "include/kernel/tty.h"
#include "string/string.h"
#include "include/kernel/sys.h"

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
    unsigned char century;
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

char *getformattedtime(const char *format) {
    char *result = kmalloc(strlen(format) + 1);
    char hourStr[3], minStr[3], secStr[3];
    // snprintf();

    sprintf(hourStr, "%02d", hour);
    sprintf(minStr, "%02d", minute);
    sprintf(secStr, "%02d", second);

    const char *p = format;
    char *q = result;
    while (*p) {
        if (*p == 'h') {
            // Hours with leading zeros
            q = strcat(q, hourStr);
        } else if (*p == 'H') {
            // Hours without leading zeros
            q = strcat(q, hour < 10 ? " " : "");
            sprintf(q, "%d", hour);
        } else if (*p == 'm') {
            // Minutes with leading zeros
            q = strcat(q, minStr);
        } else if (*p == 'M') {
            // Minutes without leading zeros
            q = strcat(q, minute < 10 ? " " : "");
            sprintf(q, "%d", minute);
        } else if (*p == 's') {
            // Seconds with leading zeros
            q = strcat(q, secStr);
        } else if (*p == 'S') {
            // Seconds without leading zeros
            q = strcat(q, second < 10 ? " " : "");
            sprintf(q, "%d", second);
        } else {
            // Copy literal characters
            char tmp[2] = {*p, '\0'};
            q = strcat(q, tmp);
        }
        p++;
    }

    return result;
}

char *gettime()
{
    char *x = kmalloc(sizeof(int) * 8);
    sprintf(x, "%d:%d:%d", hour, minute, second);
    return x;
}

void printtime()
{
    // asm volatile ("cli");
    // 	read_rtc();
    // 	asm volatile ("sti");
    read_rtc();

    printc("System Time: ", VGA_COLOR_LIGHT_GREEN);
    print(itoa(hour));
    print(":");
    print(itoa(minute));
    print(":");
    print(itoa(second));
    print("\n");
}