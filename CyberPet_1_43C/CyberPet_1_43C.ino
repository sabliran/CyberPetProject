/*
  CyberPet - 1.43C edition
  Board: Waveshare ESP32-S3-Touch-AMOLED-1.43C

  BSP (bsp_broolesia_display_init) runs lv_timer_handler() in a FreeRTOS task.
  All LVGL calls from this sketch must be wrapped in bsp_display_lock / unlock.

  RTC note: the 1.43C BSP uses ESP-IDF I2C master (not Arduino Wire), which
  conflicts with RtcTimeKeeper's Wire.h usage. Using NTP-only TimeKeeper here.
  TODO: port RtcTimeKeeper to use bsp_i2c_get_handle() to unlock onboard RTC.
*/

#include "esp32_s3_touch_amoled_1.43c.h"
#include "pet.h"
#include "habits.h"
#include "storage.h"
#include "ui.h"
#include "wifi_sync.h"
#include "timekeeping.h"

// --- Fill these in, or leave WIFI_SSID empty to run fully standalone ---
const char* WIFI_SSID     = "REDACTED_WIFI_SSID";
const char* WIFI_PASSWORD = "REDACTED_WIFI_PASSWORD";
// *.local hostnames are resolved via mDNS after WiFi connects, so this stays
// valid when the host's DHCP address changes (was http://192.168.1.8:8090).
const char* DASHBOARD_URL = "http://omarchy.local:8090";

// POSIX TZ string.  "UTC0" fires daily reset at UTC midnight.
// Example: "CET-1CEST,M3.5.0,M10.5.0/3" for Central Europe.
const char* TIMEZONE = "UTC0";

Pet pet;
HabitTracker habits;
Storage storage;
PetUI ui;
WifiSync wifiSync;
TimeKeeper timeKeeper;  // NTP-only; RTC TODO (see file header)

static int  lastResetYear = -1;
static int  lastResetDOY  = -1;
static bool ntpEnabled    = false;
static uint32_t lastDailyCheck = 0;
static const uint32_t DAY_MS   = 24UL * 60UL * 60UL * 1000UL;

static int lastXpResetToken = 0;  // last dashboard XP-reset applied (NVS-backed)
uint32_t lastSyncAttempt  = 0;
const uint32_t SYNC_INTERVAL_MS  = 60UL * 1000UL;
const uint32_t SYNC_INTERVAL_USB_MS = 10UL * 1000UL;  // faster cadence when docked over USB
uint32_t lastConfigCheck  = 0;
const uint32_t CONFIG_CHECK_MS   = 5UL * 1000UL;

void setup() {
  // USB sync bridge sends ~1 KB responses in one burst; the HWCDC default
  // RX buffer (256 B) would overflow. Must be set before Serial.begin().
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);

  // Crash forensics: the USB bridge log keeps this line, so an unexpected
  // reboot names its cause even when the panic text is lost with the port.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("boot: reset reason %d (%s)\n", (int)rr,
        rr == ESP_RST_POWERON  ? "power-on"      :
        rr == ESP_RST_SW       ? "software"       :
        rr == ESP_RST_PANIC    ? "panic"          :
        rr == ESP_RST_INT_WDT  ? "int watchdog"   :
        rr == ESP_RST_TASK_WDT ? "task watchdog"  :
        rr == ESP_RST_WDT      ? "other watchdog" :
        rr == ESP_RST_BROWNOUT ? "brownout"       : "other");
  }

  // BSP: init SH8601 QSPI LCD, CST820 touch (I2C 0x15), LVGL v8, FreeRTOS task.
  // After this call lv_timer_handler() runs in task "LVGL" — do NOT call it here.
  bsp_broolesia_display_init();  // calls bsp_batt_init() internally

  // BOOT button (GPIO0): short press opens the apps menu — see loop().
  pinMode(0, INPUT_PULLUP);

  storage.begin();

  PetState savedPet = storage.loadPet();
  pet.init(savedPet);

  PetSettings savedSettings = storage.loadSettings();
  pet.applySettings(savedSettings);

  storage.loadHabits(habits);
  if (habits.count() == 0) {
    habits.init();
  }

  // All LVGL calls need the display mutex while the FreeRTOS LVGL task runs.
  if (bsp_display_lock(-1) == ESP_OK) {
    ui.init(&pet, &habits);
    // Restore the last-synced quest/goal lists from NVS so they don't
    // vanish on reboot while waiting for the next sync.
    QuestInfo cachedQuests[MAX_QUESTS];
    GoalInfo  cachedGoals[MAX_GOALS];
    int nq = storage.loadQuests(cachedQuests);
    int ng = storage.loadGoals(cachedGoals);
    ui.setQuests(cachedQuests, nq);
    ui.setGoals(cachedGoals, ng);
    bsp_display_unlock();
  }

  if (strlen(WIFI_SSID) > 0) {
    wifiSync.begin(WIFI_SSID, WIFI_PASSWORD, DASHBOARD_URL);
    if (wifiSync.isConnected()) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      setenv("TZ", TIMEZONE, 1);
      tzset();
      ntpEnabled = true;
      wifiSync.beginEventStream();
    }
  }

  lastResetYear = storage.loadLastResetYear();
  lastResetDOY  = storage.loadLastResetDay();
  lastXpResetToken = storage.loadXpResetToken();
  lastDailyCheck  = millis();
  lastSyncAttempt = millis();
}

// After every successful sync: push results to the UI and cache the
// dashboard-owned lists in NVS (change-guarded) so they survive reboots.
static void applySyncResults() {
  if (bsp_display_lock(-1) == ESP_OK) {
    ui.refreshHabitScreen();
    ui.setQuests(wifiSync.getQuests(), wifiSync.getQuestCount());
    ui.setGoals(wifiSync.getGoals(), wifiSync.getGoalCount());
    bsp_display_unlock();
  }
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
    if (bsp_display_lock(-1) == ESP_OK) {
      ui.refreshPetScreen();
      bsp_display_unlock();
    }
  }
  lastSyncAttempt = millis();
}

static void fireDailyReset() {
  bool didAnything = habits.anyDoneToday();
  pet.dailyTick(didAnything);
  habits.resetDaily();
  storage.savePet(pet.getState());
  storage.saveHabits(habits);
  if (bsp_display_lock(-1) == ESP_OK) {
    ui.refreshPetScreen();
    bsp_display_unlock();
  }
}

void loop() {
  // ── Daily reset ───────────────────────────────────────────────────────────
  int resetHour = pet.getSettings().dailyResetHour;

  if (timeKeeper.hasSync()) {
    if (lastResetDOY == -1) {
      WallDate today = timeKeeper.now();
      if (today.valid) {
        lastResetYear = today.year;
        lastResetDOY  = today.dayOfYear;
        storage.saveLastResetYear(lastResetYear);
        storage.saveLastResetDay(lastResetDOY);
      }
    } else if (timeKeeper.shouldReset(lastResetYear, lastResetDOY, resetHour)) {
      WallDate today = timeKeeper.now();
      fireDailyReset();
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
    if (bsp_display_lock(-1) == ESP_OK) {
      ui.refreshPetScreen();
      bsp_display_unlock();
    }
  }

  // ── Change-guarded NVS save ───────────────────────────────────────────────
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
  // ui.syncStarted() already showed the spinner from the tap handler; the
  // FreeRTOS LVGL task keeps it animating while sync() blocks on HTTP here.
  {
    bool manualSync = false;
    if (bsp_display_lock(-1) == ESP_OK) {
      manualSync = ui.consumeSyncRequest();
      bsp_display_unlock();
    }
    if (manualSync) {
      // sync() tries the USB bridge first, then WiFi; works USB-only too.
      bool ok = wifiSync.sync(&pet, &habits);
      if (ok) applySyncResults();
      if (bsp_display_lock(-1) == ESP_OK) {
        ui.syncFinished(ok);
        bsp_display_unlock();
      }
    }
  }

  // ── WiFi sync ─────────────────────────────────────────────────────────────
  if (strlen(WIFI_SSID) > 0) {
    static uint32_t lastSseConnectAttempt = 0;

    if (wifiSync.isEventStreamConnected()) {
      if (wifiSync.pollEventStream(&pet, &habits)) applySyncResults();
    } else {
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

  // ── Heap telemetry (visible in the USB bridge log) ────────────────────────
  // A steady downward "min" trend across these lines = a leak; a sudden crash
  // with healthy heap = something else. Cheap enough to leave in permanently.
  {
    static uint32_t lastHeapLog = 0;
    if (millis() - lastHeapLog > 30000) {
      lastHeapLog = millis();
      Serial.printf("heap: free %u, min-ever %u\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    }
  }

  // ── BOOT button: short press = apps menu ──────────────────────────────────
  // Runtime GPIO0 is a free input (its strapping role only matters at reset).
  // No PMU on this board, so unlike the 1.75B there's no hold-to-power-off.
  {
    static uint32_t bootHeldSince = 0;
    if (digitalRead(0) == LOW) {
      if (bootHeldSince == 0) bootHeldSince = millis();
    } else if (bootHeldSince != 0) {
      uint32_t held = millis() - bootHeldSince;
      bootHeldSince = 0;
      if (held >= 50 && held < 2000) {  // 50 ms floor debounces contact bounce
        if (bsp_display_lock(-1) == ESP_OK) {
          ui.showAppsMenu();
          bsp_display_unlock();
        }
      }
    }
  }

  // ── Battery status ────────────────────────────────────────────────────────
  // 1.43C: bsp_batt_get_voltage() returns millivolts from ADC_CHANNEL_3.
  // LiPo range: 3000 mV (0%) – 4200 mV (100%).
  static uint32_t lastBattCheck = 0;
  static const uint32_t BATT_CHECK_MS = 60UL * 1000UL;
  if (millis() - lastBattCheck > BATT_CHECK_MS) {
    lastBattCheck = millis();
    uint16_t mv = bsp_batt_get_voltage();
    int pct = 0;
    if (mv >= 4200)      pct = 100;
    else if (mv <= 3000) pct = 0;
    else                 pct = (int)((uint32_t)(mv - 3000) * 100UL / 1200UL);
    bool charging = (bsp_batt_get_status() == 1);
    if (bsp_display_lock(-1) == ESP_OK) {
      ui.updateBattery(pct, charging);
      bsp_display_unlock();
    }
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}
