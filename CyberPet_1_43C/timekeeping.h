#pragma once
#include <stdint.h>  // uint8_t used by RtcTimeKeeper private helpers
// Hardware-agnostic time-source abstraction.
// No Arduino or board-specific dependencies — compiles cleanly against
// web_sim/shim/Arduino.h and on the device.
//
// timekeeping.cpp provides the firmware implementations:
//   • TimeKeeper    — NTP via POSIX time(), used as base + fallback
//   • RtcTimeKeeper — PCF85063 onboard the 1.75 (I2C addr 0x51, fixed by spec)
// Neither is in the web_sim build; main_web.cpp drives day rollover directly
// via the 'd' key.

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

    // Current local wall time.  valid=false until time source is ready.
    virtual WallDate now() const;

    // True iff now().valid.
    bool hasSync() const;

    // Returns true when:
    //   • the current calendar date is strictly later than (lastYear, lastDOY)
    //     within now().year,  AND
    //   • the current local hour >= resetHour.
    // Precondition: now().valid must be true; gate on hasSync() before calling.
    bool shouldReset(int lastYear, int lastDOY, int resetHour) const;
};

// RTC-backed time source using the PCF85063 onboard the 1.75.
//
// ⚠ Firmware-only — implemented in timekeeping.cpp, excluded from sim build.
// ⚠ Board-specific I2C access; not compile-verified off-device.
//
// Wire (I2C bus) must be started by Waveshare's board init code before
// initRtc() is called.  I2C address 0x51 is fixed by the PCF85063 spec.
//
// Time source priority:
//   1. PCF85063 RTC (valid after first NTP sync; survives power-off via coin cell)
//   2. POSIX time() NTP fallback (WiFi session before RTC has ever been set)
//   3. Invalid → caller falls back to millis()/24 h uptime path
class RtcTimeKeeper : public TimeKeeper {
public:
    // Read PCF85063 OS bit; sets rtcValid. Call from setup() after board init.
    // Logs whether the RTC holds a usable timestamp.
    void initRtc();

    // Write current POSIX time() into the RTC registers. Returns true if NTP
    // was ready (time() > 2020-01-01). No-op and returns false otherwise.
    // Call once per session after ntpEnabled and NTP has first synced.
    bool syncRtcFromNtp();

    // RTC path first, NTP fallback second (see time source priority above).
    WallDate now() const override;

private:
    bool          rtcValid  = false;
    mutable WallDate      cachedDate = {0, -1, 0, false};
    mutable unsigned long cachedAt   = 0;  // millis() timestamp of last I2C read

    uint8_t readReg(uint8_t reg) const;
    void    writeReg(uint8_t reg, uint8_t val);
    static uint8_t bcdToBin(uint8_t bcd);
    static uint8_t binToBcd(uint8_t bin);
};
