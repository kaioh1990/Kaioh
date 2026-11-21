// time.Time for the translated game: wall clock (Unix nanoseconds) plus a
// monotonic reading, as Go keeps both; durations between Now() values use
// the monotonic clock. Not used by the simulation (frame pacing, menus,
// replays, online timeouts).
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "ik/ik_sim.h"

typedef struct RtTime {
    i64 unix_ns;  // wall clock
    i64 mono_ns;  // go_mono_ns() reading (0: none)
    u8 set;       // 0: the zero Time
    u8 utc;       // UTC() / time.Unix(..).UTC(): fields in UTC, else local
} RtTime;
_Static_assert(sizeof(RtTime) <= sizeof(GoOpaque), "time.Time fits");

static GoOpaque pack(RtTime t) {
    GoOpaque o;
    memset(&o, 0, sizeof o);
    memcpy(o._opaque, &t, sizeof t);
    return o;
}
static RtTime unpack(GoOpaque o) {
    RtTime t;
    memcpy(&t, o._opaque, sizeof t);
    return t;
}

static i64 wall_ns(void) {
#if defined(_WIN32)
    // 100 ns ticks since 1601 (mingw-w64 on msvcrt has no timespec_get)
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    i64 t = (i64)(((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    return (t - 116444736000000000LL) * 100;
#else
    struct timespec ts;
#if defined(__ANDROID__)
    clock_gettime(CLOCK_REALTIME, &ts);  // timespec_get needs API 29
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (i64)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

GoOpaque time_Now(void) { return pack((RtTime){wall_ns(), go_mono_ns(), 1, 0}); }
GoOpaque time_Unix(i64 sec, i64 nsec) { return pack((RtTime){sec * 1000000000 + nsec, 0, 1, 0}); }
GoOpaque time_Time_UTC(GoOpaque t) {
    RtTime r = unpack(t);
    r.utc = 1;
    r.mono_ns = 0;
    return pack(r);
}
GoOpaque time_Time_Add(GoOpaque t, i64 d) {
    RtTime r = unpack(t);
    r.unix_ns += d;
    if (r.mono_ns) r.mono_ns += d;
    return pack(r);
}
i64 time_Time_Sub(GoOpaque a, GoOpaque b) {
    RtTime x = unpack(a), y = unpack(b);
    if (x.mono_ns && y.mono_ns) return x.mono_ns - y.mono_ns;
    return x.unix_ns - y.unix_ns;
}
i64 time_Since(GoOpaque t) { return time_Time_Sub(time_Now(), t); }
bool time_Time_After(GoOpaque a, GoOpaque b) { return time_Time_Sub(a, b) > 0; }
bool time_Time_IsZero(GoOpaque t) { return !unpack(t).set; }
i64 time_Time_Unix(GoOpaque t) {
    i64 n = unpack(t).unix_ns;
    return n >= 0 ? n / 1000000000 : -((-n + 999999999) / 1000000000);
}
i64 time_Time_UnixMilli(GoOpaque t) { return unpack(t).unix_ns / 1000000; }
i64 time_Time_UnixNano(GoOpaque t) { return unpack(t).unix_ns; }

// broken-down fields (local time unless UTC)
static struct tm fields(RtTime r, long *gmtoff) {
    time_t s = (time_t)(r.unix_ns >= 0 ? r.unix_ns / 1000000000 : -((-r.unix_ns + 999999999) / 1000000000));
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (!r.set) {  // Go's zero Time: January 1, year 1, 00:00:00 UTC
        tm.tm_year = 1 - 1900, tm.tm_mday = 1, tm.tm_wday = 1;
        if (gmtoff) *gmtoff = 0;
        return tm;
    }
#if defined(_WIN32)
    if (r.utc) gmtime_s(&tm, &s);
    else localtime_s(&tm, &s);
    if (gmtoff) {
        struct tm g;
        gmtime_s(&g, &s);
        *gmtoff = r.utc ? 0 : (long)(_mkgmtime(&tm) - _mkgmtime(&g));
    }
#else
    if (r.utc) gmtime_r(&s, &tm);
    else localtime_r(&s, &tm);
    if (gmtoff) *gmtoff = r.utc ? 0 : tm.tm_gmtoff;
#endif
    return tm;
}
int time_Time_Year(GoOpaque t) { return fields(unpack(t), NULL).tm_year + 1900; }
int time_Time_Month(GoOpaque t) { return fields(unpack(t), NULL).tm_mon + 1; }
int time_Time_Day(GoOpaque t) { return fields(unpack(t), NULL).tm_mday; }
int time_Time_Hour(GoOpaque t) { return fields(unpack(t), NULL).tm_hour; }
int time_Time_Minute(GoOpaque t) { return fields(unpack(t), NULL).tm_min; }
int time_Time_Second(GoOpaque t) { return fields(unpack(t), NULL).tm_sec; }
int time_Time_Weekday(GoOpaque t) { return fields(unpack(t), NULL).tm_wday; }

GoOpaque time_Date(int year, int month, int day, int hour, int min, int sec, int nsec, GoOpaque *loc) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = year - 1900, tm.tm_mon = month - 1, tm.tm_mday = day;
    tm.tm_hour = hour, tm.tm_min = min, tm.tm_sec = sec, tm.tm_isdst = -1;
    bool utc = loc == time_UTC;
    time_t s;
#if defined(_WIN32)
    s = utc ? _mkgmtime(&tm) : mktime(&tm);
#else
    s = utc ? timegm(&tm) : mktime(&tm);
#endif
    return pack((RtTime){(i64)s * 1000000000 + nsec, 0, 1, (u8)utc});
}

static const char *const month_names[] = {"January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};
static const char *const day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Go reference-time layouts ("2006-01-02 15:04:05", "Jan _2", ...)
GoString time_Time_Format(GoOpaque t, GoString layout) {
    RtTime r = unpack(t);
    long off = 0;
    struct tm tm = fields(r, &off);
    char out[512];
    int n = 0;
    const char *l = layout.p;
    int len = layout.len;
#define PUT(...) n += snprintf(out + n, n < (int)sizeof out ? sizeof out - (size_t)n : 0, __VA_ARGS__)
#define HAS(s) (len - i >= (int)strlen(s) && !memcmp(l + i, s, strlen(s)))
    for (int i = 0; i < len && n < (int)sizeof out - 1;) {
        int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
        if (HAS("January")) { PUT("%s", month_names[tm.tm_mon]); i += 7; }
        else if (HAS("Jan")) { PUT("%.3s", month_names[tm.tm_mon]); i += 3; }
        else if (HAS("Monday")) { PUT("%s", day_names[tm.tm_wday]); i += 6; }
        else if (HAS("Mon")) { PUT("%.3s", day_names[tm.tm_wday]); i += 3; }
        else if (HAS("MST")) { PUT("%s", r.utc ? "UTC" : "LOC"); i += 3; }
        else if (HAS("2006")) { PUT("%04d", tm.tm_year + 1900); i += 4; }
        else if (HAS("-07:00") || HAS("Z07:00")) {
            if (l[i] == 'Z' && off == 0) PUT("Z");
            else PUT("%c%02ld:%02ld", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 3600, ((off < 0 ? -off : off) / 60) % 60);
            i += 6;
        }
        else if (HAS("-0700")) { PUT("%c%02ld%02ld", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 3600, ((off < 0 ? -off : off) / 60) % 60); i += 5; }
        else if (HAS("01")) { PUT("%02d", tm.tm_mon + 1); i += 2; }
        else if (HAS("02")) { PUT("%02d", tm.tm_mday); i += 2; }
        else if (HAS("_2")) { PUT("%2d", tm.tm_mday); i += 2; }
        else if (HAS("06")) { PUT("%02d", (tm.tm_year + 1900) % 100); i += 2; }
        else if (HAS("15")) { PUT("%02d", tm.tm_hour); i += 2; }
        else if (HAS("03")) { PUT("%02d", h12); i += 2; }
        else if (HAS("04")) { PUT("%02d", tm.tm_min); i += 2; }
        else if (HAS("05")) { PUT("%02d", tm.tm_sec); i += 2; }
        else if (HAS("PM")) { PUT("%s", tm.tm_hour >= 12 ? "PM" : "AM"); i += 2; }
        else if (HAS("pm")) { PUT("%s", tm.tm_hour >= 12 ? "pm" : "am"); i += 2; }
        else if (l[i] == '.' && i + 1 < len && (l[i + 1] == '0' || l[i + 1] == '9')) {
            int d = 0;
            while (i + 1 + d < len && l[i + 1 + d] == l[i + 1]) d++;
            char frac[16];
            snprintf(frac, sizeof frac, "%09lld", (long long)((r.unix_ns % 1000000000 + 1000000000) % 1000000000));
            frac[d < 9 ? d : 9] = 0;
            if (l[i + 1] == '9') {  // trailing zeros dropped
                int k = (int)strlen(frac);
                while (k > 0 && frac[k - 1] == '0') frac[--k] = 0;
                if (k) PUT(".%s", frac);
            } else {
                PUT(".%s", frac);
            }
            i += 1 + d;
        }
        else if (l[i] == '1' && !(i + 1 < len && l[i + 1] >= '0' && l[i + 1] <= '9')) { PUT("%d", tm.tm_mon + 1); i++; }
        else if (l[i] == '2' && !(i + 1 < len && l[i + 1] >= '0' && l[i + 1] <= '9')) { PUT("%d", tm.tm_mday); i++; }
        else if (l[i] == '3') { PUT("%d", h12); i++; }
        else if (l[i] == '4') { PUT("%d", tm.tm_min); i++; }
        else if (l[i] == '5') { PUT("%d", tm.tm_sec); i++; }
        else { out[n++] = l[i++]; }
    }
#undef PUT
#undef HAS
    if (n > (int)sizeof out - 1) n = (int)sizeof out - 1;
    char *c = (char *)gc_alloc_atomic((size_t)n + 1);
    memcpy(c, out, (size_t)n);
    c[n] = 0;
    return (GoString){c, n};
}
GoString time_Time_String(GoOpaque t) { return time_Time_Format(t, GOSTR("2006-01-02 15:04:05.999999999 -0700 MST")); }
