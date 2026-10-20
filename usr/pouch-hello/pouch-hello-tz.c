// /pouch-hello-tz -- the time zone prover (patch 0060: musl's raw opens take
// openat's translation, and __map_file maps the file private).
//
// Runs POST-PIVOT (spawned as /bin/pouch-hello-tz). It writes a TZif file of
// its own at the pool root and points TZ at it: zone THY (+05:30) with one
// daylight interval, TDT (+06:30), over the first day of 1970, and the rule
// "THY-5:30" after it. musl reads the file through __map_file -- an open and a
// read-only private map -- and keeps it mapped until TZ changes. Legs, each
// printed + boot-fatal via the joey expect string:
//
//   tz-file   : tzset reads the file (tzname THY, timezone -19800); localtime
//               inside the interval is TDT 18:30 with tm_isdst set, which only
//               the file's transition table says, and after it and before it
//               THY.
//   tz-map    : the zone is exactly one row of /proc/<pid>/maps, read-only,
//               private, and the fixture's own file (devno:qid as stat says).
//   tz-unmap  : with the zone loaded, a switch to UTC removes the row
//               (musl unmaps a length that is not a page multiple).
//   tz-reload : sixteen switches back and forth leave one row, and the zone
//               answers TDT each time.
//   tz-posix  : the control: a POSIX TZ string names the same zone with no
//               file and no row, and with no daylight interval, so tz-file's
//               TDT can only have come from the file.
//
// Before 0060 the first four were red: the open was the native SYS_OPEN with
// Linux's arguments, and the map asked for MAP_SHARED, which Pouch refuses, so
// every zone file read as UTC. Each leg runs, so a red run names each failing
// leg and only those; the census line prints only when all pass. fd 1 is a
// pipe write-end joey relays to the boot log, line-buffered. Cross-compiled
// with tools/pouch-clang against the pouch sysroot.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pouch-census.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZONE "/pouch-tz-thylacine"

static int fail(const char *what) {
    printf("pouch-hello-tz: FAIL %s (errno %d)\n", what, errno);
    return 1;
}

static size_t put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
    return 4;
}

// The fixture, RFC 8536 version 2: the 32-bit block, the 64-bit block, the
// footer. Transitions at 0 (to TDT) and 86400 (back to THY).
static size_t tzif(unsigned char *buf) {
    static const int64_t trans[2] = { 0, 86400 };
    static const unsigned char idx[2] = { 1, 0 };
    static const struct { int32_t off; unsigned char dst, desig; } types[2] = {
        { 19800, 0, 0 }, { 23400, 1, 4 },
    };
    static const char desig[8] = "THY\0TDT";
    size_t n = 0;
    for (int wide = 0; wide < 2; wide++) {
        memcpy(buf + n, "TZif2", 5); n += 5;
        memset(buf + n, 0, 15); n += 15;
        n += put32(buf + n, 0); n += put32(buf + n, 0); n += put32(buf + n, 0);
        n += put32(buf + n, 2); n += put32(buf + n, 2); n += put32(buf + n, 8);
        for (int i = 0; i < 2; i++) {
            if (wide) n += put32(buf + n, (uint32_t)((uint64_t)trans[i] >> 32));
            n += put32(buf + n, (uint32_t)trans[i]);
        }
        memcpy(buf + n, idx, 2); n += 2;
        for (int i = 0; i < 2; i++) {
            n += put32(buf + n, (uint32_t)types[i].off);
            buf[n++] = types[i].dst;
            buf[n++] = types[i].desig;
        }
        memcpy(buf + n, desig, 8); n += 8;
    }
    memcpy(buf + n, "\nTHY-5:30\n", 10); n += 10;
    return n;
}

static int set_tz(const char *tz) {
    if (setenv("TZ", tz, 1) != 0) return -1;
    tzset();
    return 0;
}

// localtime at t, against the zone it should name.
static int zone_at(time_t t, int isdst, long gmtoff, const char *name, int hour, int min) {
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 0;
    return tm.tm_isdst == isdst && tm.tm_gmtoff == gmtoff && tm.tm_zone &&
           strcmp(tm.tm_zone, name) == 0 && tm.tm_hour == hour && tm.tm_min == min;
}

// The rows of /proc/<pid>/maps whose file is the fixture; *other counts those
// that are not read-only and private. -1 if the table cannot be read.
static int zone_rows(int *other) {
    struct stat st;
    if (stat(ZONE, &st) != 0) return -1;
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)getpid());
    FILE *mf = fopen(path, "r");
    if (!mf) return -1;
    int rows = 0;
    *other = 0;
    while (fgets(line, sizeof line, mf)) {
        unsigned long a = 0, b = 0, off = 0, dev = 0, qid = 0;
        char perms[8] = "", type[16] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %15s %lx:%lx", &a, &b, perms, &off, type, &dev, &qid) != 7)
            continue;
        if (dev != (unsigned long)st.st_dev || qid != (unsigned long)st.st_ino) continue;
        rows++;
        if (strcmp(perms, "r--p") != 0 || strcmp(type, "file") != 0) (*other)++;
    }
    fclose(mf);
    return rows;
}

static int leg_tz_file(void) {
    unsigned char buf[256];
    size_t len = tzif(buf);
    unlink(ZONE);
    FILE *f = fopen(ZONE, "w");
    if (!f) return fail("fopen " ZONE " for writing");
    if (fwrite(buf, 1, len, f) != len) { fclose(f); return fail("write the fixture"); }
    if (fclose(f) != 0) return fail("close the fixture");
    if (set_tz(ZONE) != 0) return fail("setenv TZ");
    if (!tzname[0] || strcmp(tzname[0], "THY") != 0 || timezone != -19800) {
        printf("pouch-hello-tz: tzname[0] %s timezone %ld\n", tzname[0] ? tzname[0] : "(null)", timezone);
        return fail("tzset did not read the zone file");
    }
    if (!zone_at(43200, 1, 23400, "TDT", 18, 30)) return fail("localtime in the interval is not TDT 18:30");
    if (!zone_at(2000000000, 0, 19800, "THY", 9, 3)) return fail("localtime after the table is not the THY rule");
    if (!zone_at(-86400, 0, 19800, "THY", 5, 30)) return fail("localtime before the table is not THY");
    return 0;
}

static int leg_tz_map(void) {
    int other;
    int rows = zone_rows(&other);
    if (rows < 0) return fail("read /proc/<pid>/maps");
    if (rows != 1 || other != 0) {
        printf("pouch-hello-tz: %d row(s) map the zone, %d not read-only private\n", rows, other);
        return fail("the zone is not one read-only private file row");
    }
    return 0;
}

static int leg_tz_unmap(void) {
    int other;
    if (zone_rows(&other) != 1) return fail("the zone is not mapped before the switch");
    if (set_tz("UTC0") != 0) return fail("setenv TZ=UTC0");
    int rows = zone_rows(&other);
    if (rows != 0) {
        printf("pouch-hello-tz: %d row(s) still map the zone after TZ=UTC0\n", rows);
        return fail("the switch did not unmap the zone");
    }
    if (!tzname[0] || strcmp(tzname[0], "UTC") != 0 || timezone != 0) return fail("TZ=UTC0 is not UTC");
    if (!zone_at(43200, 0, 0, "UTC", 12, 0)) return fail("localtime under UTC0 is not 12:00 UTC");
    return 0;
}

static int leg_tz_reload(void) {
    int other;
    for (int i = 0; i < 16; i++) {
        if (set_tz(ZONE) != 0) return fail("setenv TZ");
        if (zone_rows(&other) != 1) { printf("pouch-hello-tz: switch %d\n", i); return fail("the reload is not one row"); }
        if (!zone_at(43200, 1, 23400, "TDT", 18, 30)) { printf("pouch-hello-tz: switch %d\n", i); return fail("the reloaded zone is not TDT"); }
        if (set_tz("UTC0") != 0) return fail("setenv TZ=UTC0");
        if (zone_rows(&other) != 0) { printf("pouch-hello-tz: switch %d\n", i); return fail("the switch left a row"); }
    }
    if (set_tz(ZONE) != 0) return fail("setenv TZ");
    if (zone_rows(&other) != 1) return fail("the last reload is not one row");
    return 0;
}

static int leg_tz_posix(void) {
    int other;
    if (set_tz("THY-5:30") != 0) return fail("setenv TZ=THY-5:30");
    if (!tzname[0] || strcmp(tzname[0], "THY") != 0 || timezone != -19800) return fail("the POSIX string is not THY");
    if (!zone_at(43200, 0, 19800, "THY", 17, 30)) return fail("localtime under the POSIX string is not THY 17:30");
    int rows = zone_rows(&other);
    if (rows != 0) {
        printf("pouch-hello-tz: %d row(s) map the zone under the POSIX string\n", rows);
        return fail("the POSIX string mapped a file");
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    static const struct { const char *name; int (*run)(void); } legs[] = {
        { "tz-file", leg_tz_file },     { "tz-map", leg_tz_map },
        { "tz-unmap", leg_tz_unmap },   { "tz-reload", leg_tz_reload },
        { "tz-posix", leg_tz_posix },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; i++) {
        if (legs[i].run()) failed++;
        else printf("pouch-hello-tz: %s OK\n", legs[i].name);
    }
    unlink(ZONE);
    if (failed) {
        printf("pouch-hello-tz: %d leg(s) FAILED\n", failed);
        return 1;
    }
    puts(POUCH_CENSUS_TZ);
    return 0;
}
