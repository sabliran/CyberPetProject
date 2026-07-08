// Firmware-only time-source implementation.
// NOT compiled in the web_sim build (excluded from web_sim/CMakeLists.txt).
//
// Uses POSIX time() + localtime_r(), both available on the ESP32 Arduino core.
// The SNTP client is started in CyberPet.ino via configTime(); this file only
// reads the result.
//
// ⚠  Cannot be compile-tested without the ESP32 toolchain.  APIs used:
//      time(), localtime_r()  — POSIX <time.h>
//      configTime(), setenv(), tzset()  — called from CyberPet.ino, not here.

#include "timekeeping.h"
#include <time.h>

// Unix timestamps below this value indicate an unsynced clock.
// (ESP32 boots with ~0; NTP brings it to the real date, always > this.)
static const time_t NTP_EPOCH_MIN = 1577836800; // 2020-01-01 00:00:00 UTC

WallDate TimeKeeper::now() const {
    time_t t = time(nullptr);
    if (t < NTP_EPOCH_MIN) {
        return {0, -1, 0, false};
    }
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    return {
        local_tm.tm_year + 1900,
        local_tm.tm_yday + 1,   // tm_yday is 0-based, WallDate is 1-based
        local_tm.tm_hour,
        true
    };
}

bool TimeKeeper::hasSync() const {
    return now().valid;
}

bool TimeKeeper::shouldReset(int lastYear, int lastDOY, int resetHour) const {
    WallDate w = now();
    if (!w.valid) return false;
    // A whole new calendar year counts as a new day regardless of day-of-year.
    if (w.year > lastYear)  return w.hour >= resetHour;
    if (w.year == lastYear && w.dayOfYear > lastDOY) return w.hour >= resetHour;
    return false;
}
