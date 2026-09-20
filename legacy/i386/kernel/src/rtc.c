/* SCos native - CMOS real-time clock */
#include "scos.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg)
{
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static u8 bcd(u8 v) { return (v & 0xF) + ((v >> 4) * 10); }

void rtc_read(struct rtc_time *t)
{
    u8 sec, min, hour, day, mon, status_b;
    u16 year;
    do {
        sec  = cmos_read(0x00);
        min  = cmos_read(0x02);
        hour = cmos_read(0x04);
        day  = cmos_read(0x07);
        mon  = cmos_read(0x08);
        year = cmos_read(0x09);
    } while (sec != cmos_read(0x00));

    status_b = cmos_read(0x0B);
    u8 weekday = cmos_read(0x06);

    if (!(status_b & 0x04)) {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour & 0x7F) | (hour & 0x80);
        day = bcd(day); mon = bcd(mon); year = bcd((u8)year);
    }
    if (hour & 0x80 && !(status_b & 0x02))
        hour = ((hour & 0x7F) + 12) % 24;

    t->sec = sec; t->min = min; t->hour = hour & 0x7F;
    t->day = day; t->mon = mon;
    t->year = (u16)(year + (year < 100 ? 2000 : 0));
    t->weekday = weekday;
}

u32 rtc_to_epoch(const struct rtc_time *t)
{
    /* days from civil */
    int y = t->year - (t->mon <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp = (t->mon + 9) % 12;
    unsigned doy = (153 * mp + 2) / 5 + t->day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (u32)(days * 86400L + t->hour * 3600L + t->min * 60L + t->sec);
}
