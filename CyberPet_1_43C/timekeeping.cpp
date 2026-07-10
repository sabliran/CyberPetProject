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

// ── RtcTimeKeeper — PCF85063 onboard the 1.75 ────────────────────────────
//
// Register map (all times in BCD):
//   0x00 Control_1   0x01 Control_2
//   0x02 Seconds     (bit 7 = OS: oscillator-stopped flag)
//   0x03 Minutes     (bits 6:0)
//   0x04 Hours       (bits 5:0, 24h mode default)
//   0x05 Days        (bits 5:0)
//   0x06 Weekday     (bits 2:0)
//   0x07 Months      (bits 4:0, 1-12)
//   0x08 Years       (bits 7:0, 0-99 = 2000-2099)
//
// I2C address 0x51 is fixed by the PCF85063 spec (not a GPIO number).
// Wire is expected to be already started by Waveshare's board init before
// initRtc() is called (AXP2101/QMI8658/touch init all run on the same bus).

static const uint8_t PCF85063_ADDR = 0x51;

void RtcTimeKeeper::initRtc() {
    uint8_t secs = readReg(0x02);
    rtcValid = !(secs & 0x80);   // OS=1 means oscillator stopped

    if (rtcValid) {
        uint8_t yrs = readReg(0x08);
        int year = bcdToBin(yrs) + 2000;
        if (year < 2020) rtcValid = false;   // factory default 0x00 = year 2000
    }

    if (rtcValid) {
        cachedAt = 0;   // force fresh read in now()
        WallDate w = now();
        Serial.printf("RTC: %04d-DOY%d %02d:xx local\n", w.year, w.dayOfYear, w.hour);
    } else {
        Serial.println("RTC: oscillator stopped or stale — awaiting NTP to set time");
    }
}

bool RtcTimeKeeper::syncRtcFromNtp() {
    time_t t = time(nullptr);
    if (t < NTP_EPOCH_MIN) return false;   // NTP not ready yet

    struct tm local_tm;
    localtime_r(&t, &local_tm);

    // Writing the seconds register clears the OS bit (bit 7) in the same write.
    writeReg(0x02, binToBcd(local_tm.tm_sec));
    writeReg(0x03, binToBcd(local_tm.tm_min));
    writeReg(0x04, binToBcd(local_tm.tm_hour));            // 24h mode (Control_1 default)
    writeReg(0x05, binToBcd(local_tm.tm_mday));
    writeReg(0x06, binToBcd(local_tm.tm_wday));
    writeReg(0x07, binToBcd(local_tm.tm_mon + 1));         // PCF months are 1-12
    writeReg(0x08, binToBcd(local_tm.tm_year - 100));      // PCF years 0-99 = 2000-2099

    rtcValid = true;
    cachedAt = 0;   // invalidate cache so next now() reads fresh RTC state

    Serial.printf("RTC: set from NTP %04d-%02d-%02d %02d:%02d\n",
                  local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                  local_tm.tm_hour, local_tm.tm_min);
    return true;
}

WallDate RtcTimeKeeper::now() const {
    // 30-second read cache — daily-reset detection only needs minute resolution.
    unsigned long now_ms = millis();
    if (cachedDate.valid && (now_ms - cachedAt) < 30000UL) return cachedDate;

    if (rtcValid) {
        // Burst-read 7 registers starting at 0x02 in a single I2C transaction.
        Wire.beginTransmission(PCF85063_ADDR);
        Wire.write(0x02);
        if (Wire.endTransmission(false) == 0 &&
            Wire.requestFrom(PCF85063_ADDR, (uint8_t)7) == 7) {

            uint8_t secs   = Wire.read();   // 0x02
            uint8_t mins   = Wire.read();   // 0x03
            uint8_t hours  = Wire.read();   // 0x04
            uint8_t days   = Wire.read();   // 0x05
            /* weekday */    Wire.read();   // 0x06 (not needed for WallDate)
            uint8_t months = Wire.read();   // 0x07
            uint8_t years  = Wire.read();   // 0x08

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
                    cachedDate = {year, t.tm_yday + 1, t.tm_hour, true};
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
