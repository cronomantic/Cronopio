/* <time.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * DOOM uses time()/localtime()/strftime() for savegame timestamps and
 * screenshot names. There is no real-time clock wired to the libc: time()
 * returns 0 (epoch) and the calendar functions return a fixed 1970 date.
 * Game timing should use cron_time_ms() / cron_frame_count() in <cronopio.h>
 * instead. Everything here is 32-bit (time_t is `long`); no i64/double. */
#ifndef CVM_LIBC_TIME_H
#define CVM_LIBC_TIME_H

#include <stddef.h>
#include <sys/types.h>   /* time_t */

typedef long clock_t;
#define CLOCKS_PER_SEC 1000

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t      time(time_t *t);
clock_t     clock(void);
struct tm  *localtime(const time_t *timep);
struct tm  *gmtime(const time_t *timep);
size_t      strftime(char *s, size_t max, const char *format, const struct tm *tm);
char       *asctime(const struct tm *tm);
char       *ctime(const time_t *timep);

#endif /* CVM_LIBC_TIME_H */
