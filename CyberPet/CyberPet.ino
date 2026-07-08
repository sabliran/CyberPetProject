/*
  CyberPet - habit-tracking virtual pet for ESP32-S3-Touch-AMOLED-1.43C

  INTEGRATION NOTE:
  This sketch assumes you've dropped pet.h/.cpp, habits.h/.cpp, storage.h/.cpp,
  ui.h/.cpp, wifi_sync.h/.cpp, and timekeeping.h/.cpp into Waveshare's own
  example project for this board (github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43C,
  folder 02_Example).

  That example already contains the correct display/touch/QSPI init code for
  this exact board - copy the display + touch init calls from their
  setup() into the marked section below rather than reinventing them,
  since pin numbers and driver init sequences are board-specific.
*/

#include <lvgl.h>
#include "pet.h"
#include "habits.h"
#include "storage.h"
#include "ui.h"
#include "wifi_sync.h"
#include "timekeeping.h"

// --- Fill these in, or leave WIFI_SSID empty to run fully standalone ---
const char* WIFI_SSID     = "";           // e.g. "MyNetwork"
const char* WIFI_PASSWORD = "";           // e.g. "MyPassword"
const char* DASHBOARD_URL = "http://192.168.1.50:8090"; // docker host's LAN IP

// POSIX TZ string for your timezone.  Needed so dailyResetHour fires in local
// time after NTP sync.  Leave "UTC0" if you want resets at UTC midnight.
// Examples:
//   "EST5EDT,M3.2.0,M11.1.0"         US Eastern
//   "CET-1CEST,M3.5.0,M10.5.0/3"    Central Europe
//   "AEST-10AEDT,M10.1.0,M4.1.0/3"  Australia/Sydney
const char* TIMEZONE = "UTC0";

// --- TODO: include Waveshare's display/touch driver headers here ---
// e.g. #include "lv_port_disp.h" / their CO5300 + touch driver includes,
// copied from the 02_Example project in their repo.

Pet pet;
HabitTracker habits;
Storage storage;
PetUI ui;
WifiSync wifiSync;
TimeKeeper timeKeeper;

// Wall-clock daily-reset state (NTP path).
// Loaded from NVS in setup(); -1 means "never reset" (first boot).
static int  lastResetYear = -1;
static int  lastResetDOY  = -1;

// Whether configTime() has been called and we can try the NTP path.
// False when WIFI_SSID is blank or WiFi connection failed.
static bool ntpEnabled = false;

// Uptime fallback (standalone / NTP not yet synced).
static uint32_t lastDailyCheck = 0;
static const uint32_t DAY_MS = 24UL * 60UL * 60UL * 1000UL;

uint32_t lastSyncAttempt   = 0;
const uint32_t SYNC_INTERVAL_MS = 60UL * 1000UL;

uint32_t lastConfigCheck   = 0;
const uint32_t CONFIG_CHECK_MS = 5UL * 1000UL;

void setup() {
  Serial.begin(115200);

  // --- TODO: call Waveshare's display + touch init functions here ---
  // This must set up the LCD panel, LVGL display driver (lv_disp_drv_register),
  // and the I2C touch driver (lv_indev_drv_register) before anything below runs.
  // lv_init() is normally called inside their init function too - if not,
  // call lv_init() here first.

  storage.begin();

  PetState savedPet = storage.loadPet();
  pet.init(savedPet);

  // Restore last-configured dashboard settings (mood gain/decay, reset hour).
  // Falls back to DEFAULT_PET_SETTINGS if no NVS entry exists yet.
  PetSettings savedSettings = storage.loadSettings();
  pet.applySettings(savedSettings);

  storage.loadHabits(habits);
  if (habits.count() == 0) {
    habits.init(); // seed default habits on first boot
  }

  ui.init(&pet, &habits);

  // Optional: connect to the dashboard and start NTP.
  // Leave WIFI_SSID empty above to skip entirely and run fully standalone.
  if (strlen(WIFI_SSID) > 0) {
    wifiSync.begin(WIFI_SSID, WIFI_PASSWORD, DASHBOARD_URL);
    if (wifiSync.isConnected()) {
      // Kick off SNTP — time() will return a valid timestamp once the first
      // NTP response arrives (usually within a few seconds).
      // ⚠  configTime / setenv / tzset are ESP32 Arduino APIs; cannot be
      //    compile-tested without the ESP32 toolchain.
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      setenv("TZ", TIMEZONE, 1);
      tzset();
      ntpEnabled = true;
    }
  }

  // Load last-reset date from NVS (wall-clock path uses this to avoid
  // double-resets after a reboot that happens after midnight).
  lastResetYear = storage.loadLastResetYear();
  lastResetDOY  = storage.loadLastResetDay();

  lastDailyCheck  = millis();
  lastSyncAttempt = millis();
}

// Fire the daily reset: advance the pet, reset habits, persist, refresh UI.
// Shared by both the NTP and uptime paths.
static void fireDailyReset() {
  bool didAnything = habits.anyDoneToday();
  pet.dailyTick(didAnything);
  habits.resetDaily();
  storage.savePet(pet.getState());
  storage.saveHabits(habits);
  ui.refreshPetScreen();
}

void loop() {
  // --- TODO: call Waveshare's LVGL tick/handler functions here ---
  // Typically: lv_timer_handler(); plus a millis()-based lv_tick_inc() call,
  // exactly as shown in their example's loop().

  // ── Daily reset ──────────────────────────────────────────────────────────
  // resetHour is the local hour at which habits roll over.  After the first
  // successful sync it comes from dashboard settings; before that it's the
  // compile-time default (4 AM).
  int resetHour = pet.getSettings().dailyResetHour;

  if (ntpEnabled && timeKeeper.hasSync()) {
    // Wall-clock path: fires once per calendar day at resetHour local time.
    // Reboot-safe: lastResetDOY is persisted in NVS, so a reboot just after
    // midnight won't double-fire, and a reboot before resetHour won't skip.

    if (lastResetDOY == -1) {
      // First NTP sync ever: record today as the baseline without firing a
      // reset (the pet hasn't had a full day yet).
      WallDate today = timeKeeper.now();
      if (today.valid) {
        lastResetYear = today.year;
        lastResetDOY  = today.dayOfYear;
        storage.saveLastResetYear(lastResetYear); // change-guarded by -1 → real value
        storage.saveLastResetDay(lastResetDOY);
      }
    } else if (timeKeeper.shouldReset(lastResetYear, lastResetDOY, resetHour)) {
      WallDate today = timeKeeper.now();
      fireDailyReset();
      // Persist new date (change-guarded: only write if the value actually moved)
      if (today.year != lastResetYear) {
        storage.saveLastResetYear(today.year);
        lastResetYear = today.year;
      }
      if (today.dayOfYear != lastResetDOY) {
        storage.saveLastResetDay(today.dayOfYear);
        lastResetDOY = today.dayOfYear;
      }
    }
  } else {
    // Uptime fallback: 24 h of millis() = one day.
    // Used when WIFI_SSID is blank or NTP hasn't synced yet.
    // Matches the original behaviour exactly so standalone mode is unchanged.
    if (millis() - lastDailyCheck >= DAY_MS) {
      lastDailyCheck = millis();
      fireDailyReset();
    }
  }

  // ── Change-guarded NVS save ───────────────────────────────────────────────
  // Only writes to flash when state actually changed. No wear from idle loops.
  static uint32_t lastSaveCheck = 0;
  static PetState    lastSavedPet      = {};
  static Habit       lastSavedHabits[MAX_HABITS] = {};
  static PetSettings lastSavedSettings = DEFAULT_PET_SETTINGS;
  if (millis() - lastSaveCheck > 5000) {
    lastSaveCheck = millis();

    PetState current = pet.getState();
    if (memcmp(&current, &lastSavedPet, sizeof(PetState)) != 0) {
      storage.savePet(current);
      lastSavedPet = current;
    }
    if (memcmp(habits.habits, lastSavedHabits, sizeof(lastSavedHabits)) != 0) {
      storage.saveHabits(habits);
      memcpy(lastSavedHabits, habits.habits, sizeof(lastSavedHabits));
    }
    PetSettings curSettings = pet.getSettings();
    if (memcmp(&curSettings, &lastSavedSettings, sizeof(PetSettings)) != 0) {
      storage.saveSettings(curSettings);
      lastSavedSettings = curSettings;
    }
  }

  // ── WiFi sync ─────────────────────────────────────────────────────────────
  if (strlen(WIFI_SSID) > 0) {
    // Fast config-version poll (every 5 s) — triggers an immediate full sync
    // only when the dashboard config version has advanced (settings/habits changed).
    if (millis() - lastConfigCheck > CONFIG_CHECK_MS) {
      lastConfigCheck = millis();
      if (wifiSync.checkConfig(&pet, &habits)) {
        ui.refreshHabitScreen();
        storage.saveHabits(habits);
        lastSyncAttempt = millis();
      }
    }

    // Full sync fallback — catches pet-state push even when config didn't change.
    if (millis() - lastSyncAttempt > SYNC_INTERVAL_MS) {
      lastSyncAttempt = millis();
      if (wifiSync.sync(&pet, &habits)) {
        ui.refreshHabitScreen();
        storage.saveHabits(habits);
      }
    }
  }

  delay(5);
}
