#pragma once
// Hardware-agnostic time-source abstraction.
// No Arduino or board-specific dependencies — compiles cleanly against
// web_sim/shim/Arduino.h and on the device.
//
// timekeeping.cpp provides the firmware implementation (NTP via POSIX time()).
// It is NOT in the web_sim build; main_web.cpp drives day rollover directly
// via the 'd' key without going through this layer.
//
// To add an I2C RTC later (PCF85063, DS3231, …): subclass TimeKeeper and
// override now() — no changes required in pet/habits/CyberPet.ino logic.
// Do NOT write RTC driver code here until the I2C pads on the 1.43C are
// verified (they were unconfirmed at time of writing).

// Local hour at which the daily habit reset fires (0-23).
// Overridden at runtime by dashboard settings["dailyResetHour"] when synced.
static const int DEFAULT_DAILY_RESET_HOUR = 4; // 4 AM local time

// Snapshot of the current local wall-clock date.
// valid=false means the NTP client hasn't completed its first exchange yet;
// callers must fall back to the millis()-based uptime path.
struct WallDate {
    int  year;
    int  dayOfYear; // 1-366  (tm_yday + 1)
    int  hour;      // 0-23 local time
    bool valid;
};

// Abstract time source.  The default implementation queries POSIX time() after
// configTime() has primed the SNTP client (see timekeeping.cpp).
class TimeKeeper {
public:
    virtual ~TimeKeeper() {}

    // Current local wall time.  valid=false until NTP sync is complete.
    virtual WallDate now() const;

    // True iff now().valid — i.e. at least one SNTP response has been received.
    bool hasSync() const;

    // Returns true when:
    //   • the current calendar date is strictly later than (lastYear, lastDOY)
    //     within now().year,  AND
    //   • the current local hour >= resetHour.
    // Precondition: now().valid must be true; gate on hasSync() before calling.
    bool shouldReset(int lastYear, int lastDOY, int resetHour) const;
};
