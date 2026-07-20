/*
  CyberPet - habit-tracking virtual pet for ESP32-S3-Touch-AMOLED-1.75
  (plain variant; the -G adds GPS, not needed here)

  INTEGRATION NOTE:
  This sketch assumes you've dropped pet.h/.cpp, habits.h/.cpp, storage.h/.cpp,
  ui.h/.cpp, wifi_sync.h/.cpp, and timekeeping.h/.cpp into Waveshare's own
  example project for this board
  (github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75 - verify the example
  folder layout against that repo; it does NOT mirror the 1.43C's numbered
  01-05 structure).

  That example contains the correct display/touch/QSPI init code for
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
// Their display + capacitive-touch (I2C) driver includes, copied from the
// Arduino example in github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75.

Pet pet;
HabitTracker habits;
Storage storage;
PetUI ui;
WifiSync wifiSync;
RtcTimeKeeper timeKeeper;   // PCF85063 onboard the 1.75; falls back to NTP if RTC stale

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
const uint32_t SYNC_INTERVAL_USB_MS = 10UL * 1000UL;  // faster cadence when docked over USB
static int lastXpResetToken = 0;  // last dashboard XP-reset applied (NVS-backed)

uint32_t lastConfigCheck   = 0;
const uint32_t CONFIG_CHECK_MS = 5UL * 1000UL;

void setup() {
  // USB sync bridge sends ~1 KB responses in one burst; the HWCDC default
  // RX buffer (256 B) would overflow. Must be set before Serial.begin().
  // (ESP32-S3 USB CDC only — remove if the board routes Serial to a UART.)
  Serial.setRxBufferSize(4096);
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

  // Restore the last-synced quest/goal lists from NVS so they don't vanish
  // on reboot while waiting for the next sync.
  {
    QuestInfo cachedQuests[MAX_QUESTS];
    GoalInfo  cachedGoals[MAX_GOALS];
    int nq = storage.loadQuests(cachedQuests);
    int ng = storage.loadGoals(cachedGoals);
    ui.setQuests(cachedQuests, nq);
    ui.setGoals(cachedGoals, ng);
  }

  // Init RTC — Wire must already be running (Waveshare's board init starts it).
  // ⚠ board-specific; not compile-verified off-device.
  timeKeeper.initRtc();

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

      // Subscribe to dashboard SSE push stream for instant config reload.
      // If this fails at boot, loop() retries every 5 s.
      wifiSync.beginEventStream();
    }
  }

  // Load last-reset date from NVS (wall-clock path uses this to avoid
  // double-resets after a reboot that happens after midnight).
  lastResetYear = storage.loadLastResetYear();
  lastResetDOY  = storage.loadLastResetDay();
  lastXpResetToken = storage.loadXpResetToken();

  lastDailyCheck  = millis();
  lastSyncAttempt = millis();
}

// After every successful sync: push results to the UI and cache the
// dashboard-owned lists in NVS (change-guarded) so they survive reboots.
static void applySyncResults() {
  ui.refreshHabitScreen();
  ui.setQuests(wifiSync.getQuests(), wifiSync.getQuestCount());
  ui.setGoals(wifiSync.getGoals(), wifiSync.getGoalCount());
  storage.saveHabits(habits);
  storage.saveQuests(wifiSync.getQuests(), wifiSync.getQuestCount());
  storage.saveGoals(wifiSync.getGoals(), wifiSync.getGoalCount());

  // One-shot XP reset from the dashboard (Options tab): applied exactly once
  // per token, remembered across reboots.
  if (wifiSync.getPetResetToken() > lastXpResetToken) {
    lastXpResetToken = wifiSync.getPetResetToken();
    pet.resetProgress();
    storage.savePet(pet.getState());
    storage.saveXpResetToken(lastXpResetToken);
    ui.refreshPetScreen();
  }
  lastSyncAttempt = millis();
}

// Fire the daily reset: advance the pet, reset habits, persist, refresh UI.
// Shared by both the NTP and uptime paths.
static void fireDailyReset() {
  int done   = habits.doneTodayCount();  // both counted before resetDaily
  int missed = habits.missedToday();     // clears the flags
  pet.dailyTick(done, missed);
  habits.resetDaily();
  storage.savePet(pet.getState());
  storage.saveHabits(habits);
  ui.refreshPetScreen();
}

void loop() {
  // --- TODO: call Waveshare's LVGL tick/handler functions here ---
  // Typically: lv_timer_handler(); plus a millis()-based lv_tick_inc() call,
  // exactly as shown in the loop() of the Arduino example in
  // github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75.

  // ── One-time NTP → RTC stamp ─────────────────────────────────────────────
  // Once NTP first syncs, write the local time into the PCF85063 so future
  // standalone boots have accurate wall-clock resets without WiFi.
  static bool rtcSyncedFromNtp = false;
  if (ntpEnabled && !rtcSyncedFromNtp) {
    if (timeKeeper.syncRtcFromNtp()) {
      rtcSyncedFromNtp = true;
    }
  }

  // ── Daily reset ──────────────────────────────────────────────────────────
  // resetHour is the local hour at which habits roll over.  After the first
  // successful sync it comes from dashboard settings; before that it's the
  // compile-time default (4 AM).
  int resetHour = pet.getSettings().dailyResetHour;

  // RTC provides hasSync() even without WiFi once it has been set once.
  // Falls back to millis()/24 h only when RTC is stale AND no NTP available.
  if (timeKeeper.hasSync()) {
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

  // ── Hourly hunger decay ──────────────────────────────────────────────────
  // Hunger drains continuously (HUNGER_DECAY_PER_HOUR in pet.h) so feeding
  // via workouts is a recurring need, not a once-and-done. Uptime-based on
  // purpose: no persistence needed, and a reboot just delays one tick.
  static uint32_t lastHungerDecay = 0;
  static const uint32_t HUNGER_TICK_MS = 60UL * 60UL * 1000UL;
  if (millis() - lastHungerDecay >= HUNGER_TICK_MS) {
    lastHungerDecay = millis();
    pet.hungerHourlyTick();
    ui.refreshPetScreen();
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

  // ── Manual sync (press & hold on the pet screen) ────────────────────────
  // ui.syncStarted() already showed the spinner from the tap handler; sync()
  // blocks on HTTP here and the outcome is reported via ui.syncFinished().
  if (ui.consumeSyncRequest()) {
    // sync() tries the USB bridge first, then WiFi; works USB-only too.
    bool ok = wifiSync.sync(&pet, &habits);
    if (ok) applySyncResults();
    ui.syncFinished(ok);
  }

  // ── WiFi sync ─────────────────────────────────────────────────────────────
  if (strlen(WIFI_SSID) > 0) {
    static uint32_t lastSseConnectAttempt = 0;

    if (wifiSync.isEventStreamConnected()) {
      // SSE stream is live: read any pending events (non-blocking).
      // Triggers sync() immediately when "event: config" arrives.
      if (wifiSync.pollEventStream(&pet, &habits)) applySyncResults();
    } else {
      // SSE down: fall back to 5 s config-version poll for near-instant pickup
      // and retry opening the stream every 5 s.
      if (millis() - lastConfigCheck > CONFIG_CHECK_MS) {
        lastConfigCheck = millis();
        if (wifiSync.checkConfig(&pet, &habits)) applySyncResults();
      }
      if (millis() - lastSseConnectAttempt > 5000) {
        lastSseConnectAttempt = millis();
        wifiSync.beginEventStream();
      }
    }

  }

  // Full sync — every 10 s while a USB host is attached (cheap local bridge),
  // every 60 s over WiFi (sync() itself prefers the USB bridge over WiFi HTTP).
  uint32_t syncInterval = wifiSync.usbAvailable() ? SYNC_INTERVAL_USB_MS : SYNC_INTERVAL_MS;
  if ((strlen(WIFI_SSID) > 0 || wifiSync.usbAvailable()) &&
      millis() - lastSyncAttempt > syncInterval) {
    lastSyncAttempt = millis();
    if (wifiSync.sync(&pet, &habits)) applySyncResults();
  }

  // ── Battery status ────────────────────────────────────────────────────────
  // TODO: replace stub with real AXP2101 I2C read.
  // Driver + I2C init come from the 1.75 example at:
  //   github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75
  // (board-specific; not compile-verified off-device)
  // Example call once driver is initialised:
  //   int battPct = axp.getBatteryPercent();  // or equivalent from Waveshare's driver
  //   ui.updateBattery(battPct);
  static uint32_t lastBattCheck = 0;
  static const uint32_t BATT_CHECK_MS = 60UL * 1000UL;
  if (millis() - lastBattCheck > BATT_CHECK_MS) {
    lastBattCheck = millis();
    // ui.updateBattery(axp.getBatteryPercent());
  }

  delay(5);
}
