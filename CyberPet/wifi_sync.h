#pragma once
#include <Arduino.h>
#include "pet.h"
#include "habits.h"

// Handles periodic sync between the device and the CyberPet dashboard's
// REST API (see CyberPetDashboard/ - the Docker web control panel).
//
// This is intentionally simple: the device is the source of truth for
// streaks/completion history, the dashboard is the source of truth for
// which habits/goals/settings exist. Each sync call reconciles both.
class WifiSync {
public:
  // ssid/password: your WiFi network
  // serverUrl: e.g. "http://192.168.1.50:8080" - your machine running the
  //            docker container, NOT localhost (the device isn't the host)
  void begin(const char* ssid, const char* password, const char* serverUrl);

  bool isConnected();

  // Call periodically from loop() (respect settings.syncIntervalSeconds,
  // default every 60s - don't hammer the API every loop iteration)
  // Pulls habit/goal/settings changes down, pushes pet state + completions up.
  // Dashboard settings (moodGainPerHabit, moodDecayPerMiss, dailyResetHour)
  // are applied via pet->applySettings() and read back via pet->getSettings().
  bool sync(Pet* pet, HabitTracker* tracker);

  // Call every ~5 s from loop(). Hits GET /api/config-version (tiny response).
  // Returns true and calls sync() immediately when the server config version
  // has advanced since last check — gives near-instant config pickup without
  // hammering the full sync endpoint on every tick.
  bool checkConfig(Pet* pet, HabitTracker* tracker);

private:
  String serverUrl;
  bool connected = false;
  int  lastKnownConfigVersion = -1; // -1 forces a sync on the very first check

  // Habit reconciliation (see sync() in wifi_sync.cpp):
  //   1. Match by Habit::serverId == server habit.id  (rename-safe)
  //   2. Fallback: truncation-aware strncmp on name   (first sync / legacy)
  //   On name-fallback match, the server id is adopted so future syncs use (1).
  // Names longer than HABIT_NAME_LEN-1 chars are truncated on both compare and
  // store, so long names sync stably (just truncated).
};
