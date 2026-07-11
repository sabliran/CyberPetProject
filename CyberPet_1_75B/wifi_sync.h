#pragma once
#include <Arduino.h>
#include <WiFi.h>      // WiFiClient (firmware-only; this header is never in the sim build)
#include "pet.h"
#include "habits.h"
#include "ui.h"        // QuestInfo / GoalInfo (parsed from the sync response)

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
  // has advanced since last check. Used as fallback when SSE stream is down.
  bool checkConfig(Pet* pet, HabitTracker* tracker);

  // Open a persistent GET /api/events SSE connection to the dashboard.
  // Call once after begin() succeeds; the stream is maintained across loop()
  // calls via pollEventStream(). Returns true if the connection is up.
  // Blocking on first call (up to ~3 s for TCP + HTTP headers); safe to
  // call every 5 s from loop() when the stream is disconnected.
  bool beginEventStream();

  // Non-blocking. Reads any pending bytes from the SSE stream and triggers
  // sync() immediately if an "event: config" event arrives. Returns true if
  // a sync was performed. Call every loop() iteration when isEventStreamConnected().
  bool pollEventStream(Pet* pet, HabitTracker* tracker);

  bool isEventStreamConnected() const { return sseConnected; }

  // True when a host has the USB CDC serial port open. sync() prefers the
  // USB bridge (CyberPetDashboard/usb-bridge.py on the host) over WiFi HTTP:
  // it works with no WiFi configured and avoids LAN/firewall issues.
  bool usbAvailable() const { return (bool)Serial; }

  // Quests parsed from the last successful sync() response (active, not done).
  // Read-only display data; pass to ui.setQuests() after each sync.
  const QuestInfo* getQuests() const { return quests; }
  int getQuestCount() const { return questCount; }

  // Goals parsed from the last successful sync() response (active only).
  // Same contract as quests; pass to ui.setGoals() after each sync.
  const GoalInfo* getGoals() const { return goals; }
  int getGoalCount() const { return goalCount; }

  // Same contract as quests; pass to ui.setTrophies() after each sync.
  const TrophyInfo* getTrophies() const { return trophies; }
  int getTrophyCount() const { return trophyCount; }

  // One-shot commands ride the sync response as monotonic tokens: the server
  // increments petResetToken when the user presses "Reset XP" on the
  // dashboard; loop() compares against the NVS-persisted last-applied token
  // and calls pet->resetProgress() exactly once.
  int getPetResetToken() const { return petResetToken; }

  // Walk app: today's step count + the device-local calendar date it belongs
  // to (year 0 = clock not valid yet). Included in every sync request so the
  // dashboard can accumulate per-day walking history. Call whenever the
  // count changes; cheap (just stores members).
  void setStepInfo(uint32_t stepsToday, int year, int dayOfYear) {
    stepsToday_ = stepsToday; stepYear_ = year; stepDoy_ = dayOfYear;
  }

  // Sleep app: the last logged rating and the calendar date it was logged
  // on. quality -1 = never logged (omitted from the sync request); the
  // server only records entries with a valid date stamp.
  void setSleepInfo(int quality, int year, int dayOfYear) {
    sleepQuality_ = quality; sleepYear_ = year; sleepDoy_ = dayOfYear;
  }

  // Back-workout app: lifetime completed sessions (server keeps the max;
  // used for trophies).
  void setBackSessions(uint32_t n) { backSessions_ = n; }

private:
  uint32_t  stepsToday_ = 0;
  int       stepYear_   = 0;
  int       stepDoy_    = 0;
  int       sleepQuality_ = -1;
  int       sleepYear_    = 0;
  int       sleepDoy_     = 0;
  uint32_t  backSessions_ = 0;
  QuestInfo quests[MAX_QUESTS];
  int       questCount = 0;
  GoalInfo  goals[MAX_GOALS];
  int       goalCount = 0;
  TrophyInfo trophies[MAX_TROPHIES];
  int       trophyCount = 0;
  int       petResetToken = 0;

  // Shared sync plumbing: the request body and response handling are
  // transport-agnostic; sync() picks USB or HTTP to move the bytes.
  void buildSyncRequest(Pet* pet, HabitTracker* tracker, String& out);
  bool applySyncResponse(const String& response, Pet* pet, HabitTracker* tracker);
  bool usbTransact(const String& body, String& resp);

  // mDNS: a "*.local" host in the configured URL is resolved to an IP once
  // after WiFi connects (plain DNS can't see .local names), and re-resolved
  // after an HTTP connection failure in case the host's DHCP lease moved.
  void resolveServerUrl();
  String serverUrlConfigured;  // as passed to begin(); may contain *.local
  bool   mdnsStarted = false;
  String serverUrl;            // resolved form actually used for requests
  bool connected = false;
  int  lastKnownConfigVersion = -1; // -1 forces a sync on the very first check

  // SSE push stream state
  WiFiClient sseClient;
  bool       sseConnected = false;
  uint32_t   sseLastPing  = 0;   // millis() of last received byte; used for stale detection
  String     sseLineBuf;         // accumulates chars until '\n'

  // Habit reconciliation (see sync() in wifi_sync.cpp):
  //   1. Match by Habit::serverId == server habit.id  (rename-safe)
  //   2. Fallback: truncation-aware strncmp on name   (first sync / legacy)
  //   On name-fallback match, the server id is adopted so future syncs use (1).
  // Names longer than HABIT_NAME_LEN-1 chars are truncated on both compare and
  // store, so long names sync stably (just truncated).
};
