/*
 * system_info.h - Linux system information readers used by the server
 * child process (reads /proc, sysinfo(), uname(), gethostname()).
 *
 * Every function returns 0 on success and -1 on failure (errno may be
 * set).  On failure the output structure must be treated as undefined
 * and the caller replies INTERNAL_ERROR (SPEC RB-10).
 */
#ifndef LSM_SYSTEM_INFO_H
#define LSM_SYSTEM_INFO_H

#define SYSMON_NAME_LEN 128

struct sysmon_cpu {
    long   cores;        /* logical cores online                      */
    double usage_pct;    /* overall utilisation over the sample window */
};

struct sysmon_memory {
    unsigned long long total_kb;
    unsigned long long available_kb;
};

struct sysmon_host {
    char hostname[SYSMON_NAME_LEN];
    char os[SYSMON_NAME_LEN];       /* uname.sysname  e.g. Linux        */
    char kernel[SYSMON_NAME_LEN];   /* uname.release  e.g. 5.4.0-150    */
};

/* Samples /proc/stat twice (about 200 ms apart) and computes
 * (dTotal - dIdle) / dTotal * 100.  Handles dTotal == 0. */
int sysmon_get_cpu(struct sysmon_cpu *out);

/* MemTotal / MemAvailable from /proc/meminfo. */
int sysmon_get_memory(struct sysmon_memory *out);

/* Seconds since boot from /proc/uptime, falling back to sysinfo(). */
int sysmon_get_uptime(long long *uptime_sec);

/* gethostname() + uname(); values are sanitised to single tokens. */
int sysmon_get_host(struct sysmon_host *out);

#endif /* LSM_SYSTEM_INFO_H */
