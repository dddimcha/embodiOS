/* Kernel stub for time.h
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_TIME_H
#define _COMPAT_TIME_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Time types */
typedef long time_t;
typedef long clock_t;
typedef long suseconds_t;

struct tm {
    int tm_sec;     /* Seconds (0-60) */
    int tm_min;     /* Minutes (0-59) */
    int tm_hour;    /* Hours (0-23) */
    int tm_mday;    /* Day of month (1-31) */
    int tm_mon;     /* Month (0-11) */
    int tm_year;    /* Year - 1900 */
    int tm_wday;    /* Day of week (0-6) */
    int tm_yday;    /* Day of year (0-365) */
    int tm_isdst;   /* Daylight saving time */
};

struct timespec {
    time_t tv_sec;  /* Seconds */
    long   tv_nsec; /* Nanoseconds */
};

struct timeval {
    time_t      tv_sec;  /* Seconds */
    suseconds_t tv_usec; /* Microseconds */
};

/* Clock constants */
#define CLOCKS_PER_SEC 1000000
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

/* Time functions - most are stubs */
time_t time(time_t* tloc);
clock_t clock(void);
double difftime(time_t time1, time_t time0);
time_t mktime(struct tm* tm);

/* Clock functions */
int clock_gettime(int clock_id, struct timespec* tp);
int clock_getres(int clock_id, struct timespec* res);

/* Conversion functions - stubs */
struct tm* gmtime(const time_t* timep);
struct tm* gmtime_r(const time_t* timep, struct tm* result);
struct tm* localtime(const time_t* timep);
struct tm* localtime_r(const time_t* timep, struct tm* result);
char* asctime(const struct tm* tm);
char* asctime_r(const struct tm* tm, char* buf);
char* ctime(const time_t* timep);
char* ctime_r(const time_t* timep, char* buf);
size_t strftime(char* s, size_t max, const char* format, const struct tm* tm);

/* High-resolution timing (kernel-specific) */
uint64_t get_ticks(void);
uint64_t get_ticks_per_second(void);

/* Sleep functions - busy wait in kernel */
int nanosleep(const struct timespec* req, struct timespec* rem);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_TIME_H */
