// Firmware-only time-source implementations.
// NOT compiled in the web_sim build (excluded from web_sim/CMakeLists.txt).
//
// ⚠  Cannot be compile-tested without the ESP32 toolchain.  APIs used:
//      time(), localtime_r()  — POSIX <time.h>
//      Wire.beginTransmission/write/endTransmission/requestFrom/read — <Wire.h>
//      configTime(), setenv(), tzset()  — called from CyberPet.ino, not here.
//      Serial.print/printf               — Arduino

#include "timekeeping.h"
#include <time.h>
#include <Wire.h>    // I2C for PCF85063 (RtcTimeKeeper)
#include <Arduino.h> // millis(), Serial

// Unix timestamps below this value indicate an unsynced clock.
// (ESP32 boots with ~0; NTP brings it to the real date, always > this.)
static const time_t NTP_EPOCH_MIN = 1577836800; // 2020-01-01 00:00:00 UTC

WallDate TimeKeeper::now() const {
    time_t t = time(nullptr);
    if (t < NTP_EPOCH_MIN) {
        return {0, -1, 0, 0, false};
    }
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    return {
        local_tm.tm_year + 1900,
        local_tm.tm_yday + 1,   // tm_yday is 0-based, WallDate is 1-based
        local_tm.tm_hour,
        local_tm.tm_min,
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

// ── RtcTimeKeeper — PCF85063A onboard the 1.75 ───────────────────────────
//
// ⚠ This board carries the **A variant**, whose time registers start at
// 0x04 — NOT 0x02 like the plain PCF85063. Verified on hardware: writing
// 0x26 to reg 0x08 reads back 0x06, the 3-bit mask of the A-map's weekday
// register (the old map wrote the time into Offset/RAM/wrong registers and
// silently never kept time).
//
// PCF85063A register map (all times in BCD):
//   0x00 Control_1   0x01 Control_2
//   0x02 Offset      (crystal ppm correction — keep 0)
//   0x03 RAM_byte
//   0x04 Seconds     (bit 7 = OS: oscillator-stopped flag)
//   0x05 Minutes     (bits 6:0)
//   0x06 Hours       (bits 5:0, 24h mode default)
//   0x07 Days        (bits 5:0)
//   0x08 Weekday     (bits 2:0)
//   0x09 Months      (bits 4:0, 1-12)
//   0x0A Years       (bits 7:0, 0-99 = 2000-2099)
//
// I2C address 0x51 is fixed by the PCF85063 spec (not a GPIO number).
// Wire is expected to be already started by Waveshare's board init before
// initRtc() is called (AXP2101/QMI8658/touch init all run on the same bus).

static const uint8_t PCF85063_ADDR = 0x51;
static const uint8_t REG_OFFSET    = 0x02;
static const uint8_t REG_SECONDS   = 0x04;
static const uint8_t REG_YEARS     = 0x0A;

void RtcTimeKeeper::initRtc() {
    uint8_t secs = readReg(REG_SECONDS);
    rtcValid = !(secs & 0x80);   // OS=1 means oscillator stopped

    if (rtcValid) {
        uint8_t yrs = readReg(REG_YEARS);
        int year = bcdToBin(yrs) + 2000;
        if (year < 2020) rtcValid = false;   // factory default 0x00 = year 2000
    }

    if (rtcValid) {
        cachedAt = 0;   // force fresh read in now()
        WallDate w = now();
        Serial.printf("RTC: %04d-DOY%d %02d:xx local\n", w.year, w.dayOfYear, w.hour);
    } else {
        // secs 0xFF = nothing ACKed at 0x51 (readReg failure value): the
        // board has no reachable RTC and the POSIX/NTP clock is the real
        // time source. Any other value = genuine stopped/stale oscillator.
        Serial.printf("RTC: oscillator stopped or stale — awaiting NTP to set time (secs reg 0x%02X)\n", secs);
    }
}

bool RtcTimeKeeper::syncRtcFromNtp() {
    time_t t = time(nullptr);
    if (t < NTP_EPOCH_MIN) return false;   // NTP not ready yet

    struct tm local_tm;
    localtime_r(&t, &local_tm);

    // Repair the Offset register: the old (non-A) register map wrote BCD
    // seconds into it, detuning the crystal's ppm correction. Harmless to
    // clear on every set.
    writeReg(REG_OFFSET, 0x00);

    // Writing the seconds register clears the OS bit (bit 7) in the same write.
    writeReg(0x04, binToBcd(local_tm.tm_sec));
    writeReg(0x05, binToBcd(local_tm.tm_min));
    writeReg(0x06, binToBcd(local_tm.tm_hour));            // 24h mode (Control_1 default)
    writeReg(0x07, binToBcd(local_tm.tm_mday));
    writeReg(0x08, binToBcd(local_tm.tm_wday));
    writeReg(0x09, binToBcd(local_tm.tm_mon + 1));         // PCF months are 1-12
    writeReg(0x0A, binToBcd(local_tm.tm_year - 100));      // PCF years 0-99 = 2000-2099

    // Verify the write landed: writeReg ignores NACKs, so without this a
    // missing/unpowered RTC chip makes the whole path a silent no-op (the
    // "set from NTP" log would be a lie). NTP keeps the clock either way.
    uint8_t vYrs = readReg(REG_YEARS);
    if (bcdToBin(vYrs) != (uint8_t)(local_tm.tm_year - 100)) {
        Serial.printf("RTC: write verify FAILED (year reg 0x%02X) — no RTC chip? NTP clock still active\n", vYrs);
        rtcValid = false;
        return true;   // NTP was ready; don't retry a dead chip every loop
    }

    rtcValid = true;
    cachedAt = 0;   // invalidate cache so next now() reads fresh RTC state

    Serial.printf("RTC: set from NTP %04d-%02d-%02d %02d:%02d (verified)\n",
                  local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                  local_tm.tm_hour, local_tm.tm_min);
    return true;
}

WallDate RtcTimeKeeper::now() const {
    // 10-second read cache — short enough that the pet-screen clock flips
    // within moments of the real minute; still trivial I2C traffic.
    unsigned long now_ms = millis();
    if (cachedDate.valid && (now_ms - cachedAt) < 10000UL) return cachedDate;

    if (rtcValid) {
        // Burst-read 7 registers starting at Seconds in a single I2C transaction.
        Wire.beginTransmission(PCF85063_ADDR);
        Wire.write(REG_SECONDS);
        if (Wire.endTransmission(false) == 0 &&
            Wire.requestFrom(PCF85063_ADDR, (uint8_t)7) == 7) {

            uint8_t secs   = Wire.read();   // 0x04
            uint8_t mins   = Wire.read();   // 0x05
            uint8_t hours  = Wire.read();   // 0x06
            uint8_t days   = Wire.read();   // 0x07
            /* weekday */    Wire.read();   // 0x08 (not needed for WallDate)
            uint8_t months = Wire.read();   // 0x09
            uint8_t years  = Wire.read();   // 0x0A

            if (!(secs & 0x80)) {   // OS bit clear = time is reliable
                struct tm t   = {};
                t.tm_sec      = bcdToBin(secs   & 0x7F);
                t.tm_min      = bcdToBin(mins   & 0x7F);
                t.tm_hour     = bcdToBin(hours  & 0x3F);
                t.tm_mday     = bcdToBin(days   & 0x3F);
                t.tm_mon      = bcdToBin(months & 0x1F) - 1;   // tm_mon is 0-based
                t.tm_year     = bcdToBin(years)          + 100; // tm_year since 1900
                t.tm_isdst    = -1;
                mktime(&t);   // normalises and fills tm_yday

                int year = t.tm_year + 1900;
                if (year >= 2020) {
                    cachedDate = {year, t.tm_yday + 1, t.tm_hour, t.tm_min, true};
                    cachedAt   = now_ms;
                    return cachedDate;
                }
            }
        }
        // I2C error or OS bit set — fall through to NTP path
    }

    // NTP fallback (same as base TimeKeeper::now())
    WallDate base = TimeKeeper::now();
    if (base.valid) {
        cachedDate = base;
        cachedAt   = now_ms;
    }
    return base;
}

uint8_t RtcTimeKeeper::readReg(uint8_t reg) const {
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(PCF85063_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

void RtcTimeKeeper::writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

uint8_t RtcTimeKeeper::bcdToBin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

uint8_t RtcTimeKeeper::binToBcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}
