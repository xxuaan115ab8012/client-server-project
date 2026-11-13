/*
 * system_info.c - Linux system information readers (server side)
 */
#include "system_info.h"
#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define CPU_SAMPLE_INTERVAL_MS 200

struct cpu_times {
    unsigned long long total;
    unsigned long long idle;
};

/* Parse the aggregate "cpu ..." line of /proc/stat. Returns 0 / -1. */
static int read_cpu_times(struct cpu_times *out)
{
    FILE *fp = fopen("/proc/stat", "r");
    char line[512];
    unsigned long long v[10];
    int n;

    if (fp == NULL)
        return -1;
    if (fgets(line, sizeof line, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    memset(v, 0, sizeof v);
    /* user nice system idle iowait irq softirq steal guest guest_nice */
    n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
               &v[8], &v[9]);
    if (n < 4)
        return -1;

    out->idle = v[3] + v[4];               /* idle + iowait */
    out->total = 0;
    for (int i = 0; i < 8 && i < n; i++)   /* exclude guest counters */
        out->total += v[i];
    return 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

int sysmon_get_cpu(struct sysmon_cpu *out)
{
    struct cpu_times a, b;
    unsigned long long d_total, d_idle;
    long cores;

    if (out == NULL)
        return -1;

    cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1)
        return -1;

    if (read_cpu_times(&a) != 0)
        return -1;
    sleep_ms(CPU_SAMPLE_INTERVAL_MS);
    if (read_cpu_times(&b) != 0)
        return -1;

    if (b.total < a.total || b.idle < a.idle)
        return -1;                    /* counters went backwards */
    d_total = b.total - a.total;
    d_idle = b.idle - a.idle;

    out->cores = cores;
    if (d_total == 0) {
        out->usage_pct = 0.0;         /* no ticks elapsed: report idle */
    } else {
        if (d_idle > d_total)
            d_idle = d_total;
        out->usage_pct = (double)(d_total - d_idle) * 100.0 / (double)d_total;
    }
    return 0;
}

int sysmon_get_memory(struct sysmon_memory *out)
{
    FILE *fp;
    char line[256];
    int have_total = 0, have_avail = 0;

    if (out == NULL)
        return -1;
    fp = fopen("/proc/meminfo", "r");
    if (fp == NULL)
        return -1;

    while (fgets(line, sizeof line, fp) != NULL) {
        unsigned long long val;
        if (!have_total && sscanf(line, "MemTotal: %llu kB", &val) == 1) {
            out->total_kb = val;
            have_total = 1;
        } else if (!have_avail &&
                   sscanf(line, "MemAvailable: %llu kB", &val) == 1) {
            out->available_kb = val;
            have_avail = 1;
        }
        if (have_total && have_avail)
            break;
    }
    fclose(fp);
    return (have_total && have_avail) ? 0 : -1;
}

int sysmon_get_uptime(long long *uptime_sec)
{
    FILE *fp;
    double up = 0.0;
    struct sysinfo si;

    if (uptime_sec == NULL)
        return -1;

    fp = fopen("/proc/uptime", "r");
    if (fp != NULL) {
        int n = fscanf(fp, "%lf", &up);
        fclose(fp);
        if (n == 1 && up >= 0.0) {
            *uptime_sec = (long long)up;
            return 0;
        }
    }
    /* Fallback */
    if (sysinfo(&si) != 0)
        return -1;
    *uptime_sec = (long long)si.uptime;
    return 0;
}

static int copy_token(char *dst, size_t cap, const char *src)
{
    int n = snprintf(dst, cap, "%s", src);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    proto_sanitize_value(dst);
    return 0;
}

int sysmon_get_host(struct sysmon_host *out)
{
    struct utsname u;
    char hn[SYSMON_NAME_LEN];

    if (out == NULL)
        return -1;

    memset(hn, 0, sizeof hn);
    if (gethostname(hn, sizeof hn - 1) != 0 || hn[0] == '\0')
        return -1;
    hn[sizeof hn - 1] = '\0';

    if (uname(&u) != 0)
        return -1;

    if (copy_token(out->hostname, sizeof out->hostname, hn) != 0 ||
        copy_token(out->os, sizeof out->os, u.sysname) != 0 ||
        copy_token(out->kernel, sizeof out->kernel, u.release) != 0)
        return -1;
    return 0;
}
