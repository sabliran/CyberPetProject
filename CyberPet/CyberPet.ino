/*
  CyberPet - habit-tracking virtual pet for ESP32-S3-Touch-AMOLED-1.43C

  INTEGRATION NOTE:
  This sketch assumes you've dropped pet.h/.cpp, habits.h/.cpp, storage.h/.cpp,
  and ui.h/.cpp into Waveshare's own example project for this board (from
  github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43C, folder 02_Example).

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

// --- Fill these in, or leave WIFI_SSID empty to run fully standalone ---
const char* WIFI_SSID = "";           // e.g. "MyNetwork"
const char* WIFI_PASSWORD = "";       // e.g. "MyPassword"
const char* DASHBOARD_URL = "http://192.168.1.50:8090"; // your docker host's LAN IP, not localhost

// --- TODO: include Waveshare's display/touch driver headers here ---
// e.g. #include "lv_port_disp.h" / their CO5300 + touch driver includes,
// copied from the 02_Example project in their repo.

Pet pet;
HabitTracker habits;
Storage storage;
PetUI ui;
WifiSync wifiSync;

uint32_t lastDailyCheck = 0;
const uint32_t DAY_MS = 24UL * 60UL * 60UL * 1000UL;

uint32_t lastSyncAttempt   = 0;
const uint32_t SYNC_INTERVAL_MS = 60UL * 1000UL; // full sync fallback interval

uint32_t lastConfigCheck   = 0;
const uint32_t CONFIG_CHECK_MS = 5UL * 1000UL;   // poll config-version every 5 s

void setup() {
  Serial.begin(115200);

  // --- TODO: call Waveshare's display + touch init functions here ---
  // This must set up the LCD panel, LVGL display driver (lv_disp_drv_register),
  // and the I2C touch driver (lv_indev_drv_register) before anything below runs.
  // lv_init() is normally called inside their init function too - if not,
  // call lv_init() here first.

  storage.begin();

  // Load saved pet + habit state from flash (survives power loss/reboot)
  PetState savedPet = storage.loadPet();
  pet.init(savedPet);

  storage.loadHabits(habits);
  if (habits.count() == 0) {
    habits.init(); // seed default habits on first boot
  }

  ui.init(&pet, &habits);

  // Optional: connect to the dashboard for hybrid sync. Leave WIFI_SSID
  // empty above to skip this entirely and run fully standalone.
  if (strlen(WIFI_SSID) > 0) {
    wifiSync.begin(WIFI_SSID, WIFI_PASSWORD, DASHBOARD_URL);
  }

  lastDailyCheck = millis();
  lastSyncAttempt = millis();
}

void loop() {
  // --- TODO: call Waveshare's LVGL tick/handler functions here ---
  // Typically: lv_timer_handler(); plus a millis()-based lv_tick_inc() call,
  // exactly as shown in their example's loop().

  // Simple day-rollover check (uses uptime, not wall-clock time).
  // For accurate midnight resets, wire this to the onboard PCF85063 RTC
  // instead - read its date once per loop and compare day-of-year to
  // storage.loadLastResetDay(). Left as uptime-based for a working MVP.
  if (millis() - lastDailyCheck >= DAY_MS) {
    lastDailyCheck = millis();

    bool didAnything = habits.anyDoneToday();
    pet.dailyTick(didAnything);
    habits.resetDaily();

    storage.savePet(pet.getState());
    storage.saveHabits(habits);

    ui.refreshPetScreen();
  }

  // Persist state, but only when it actually changed - NVS flash is
  // wear-leveled but there's no reason to rewrite identical bytes every
  // few seconds for the lifetime of the device.
  static uint32_t lastSaveCheck = 0;
  static PetState lastSavedPet = {};
  static Habit lastSavedHabits[MAX_HABITS] = {};
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
  }

  if (strlen(WIFI_SSID) > 0) {
    // Fast config-version poll (every 5 s) — triggers an immediate full sync
    // only when the dashboard config version has advanced (settings/habits changed).
    // This is the push-style pickup; the periodic sync below is the fallback.
    if (millis() - lastConfigCheck > CONFIG_CHECK_MS) {
      lastConfigCheck = millis();
      if (wifiSync.checkConfig(&pet, &habits)) {
        ui.refreshHabitScreen();
        storage.saveHabits(habits);
        lastSyncAttempt = millis(); // reset fallback timer so we don't double-sync
      }
    }

    // Full sync fallback — catches pet-state push even when config didn't change
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
