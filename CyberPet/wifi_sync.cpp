#include "wifi_sync.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // install via Library Manager: "ArduinoJson" by Benoit Blanchon
// NOTE: written against ArduinoJson v6 (StaticJsonDocument). In v7 the
// class is just JsonDocument - a two-line rename if you're on v7.
//
// ⚠  Cannot be compile-tested without the ESP32 toolchain.

void WifiSync::begin(const char* ssid, const char* password, const char* server) {
  serverUrl = String(server);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  connected = (WiFi.status() == WL_CONNECTED);
  if (connected) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed - device will keep running standalone.");
  }
}

bool WifiSync::isConnected() {
  connected = (WiFi.status() == WL_CONNECTED);
  return connected;
}

bool WifiSync::sync(Pet* pet, HabitTracker* tracker) {
  if (!isConnected()) return false;

  HTTPClient http;
  http.begin(serverUrl + "/api/sync");
  http.addHeader("Content-Type", "application/json");

  // Build request body.
  // completedHabits: array of {id, name} objects.  id is the dashboard's
  // habit.id (Habit::serverId); -1 when the habit has never been synced.
  // The server uses id for accurate log attribution; name is kept alongside
  // so old-format servers (string-only) can still parse the completion.
  StaticJsonDocument<2048> reqDoc;
  reqDoc["deviceId"] = WiFi.macAddress();

  PetState p = pet->getState();
  JsonObject petObj = reqDoc.createNestedObject("petState");
  petObj["xp"]       = p.xp;
  petObj["stage"]    = p.stage;
  petObj["mood"]     = p.mood;
  petObj["daysAlive"]= p.daysAlive;
  petObj["hunger"]   = p.hunger;
  petObj["alive"]    = p.alive;

  JsonArray completed = reqDoc.createNestedArray("completedHabits");
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (h->active && h->doneToday) {
      JsonObject entry = completed.createNestedObject();
      entry["id"]   = h->serverId; // -1 when not yet matched to a server habit
      entry["name"] = h->name;
    }
  }

  String body;
  serializeJson(reqDoc, body);

  int httpCode = http.POST(body);
  if (httpCode != 200) {
    Serial.printf("Sync failed, HTTP %d\n", httpCode);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  // Response contains { habits, goals, settings, dashXpTotal, configVersion }.
  DynamicJsonDocument respDoc(6144);
  DeserializationError err = deserializeJson(respDoc, response);
  if (err) {
    Serial.print("Sync response parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  // ── Habit reconciliation: id-first, name-fallback ─────────────────────────
  //
  // Pass 1: for each server habit, find the matching on-device slot.
  //   • If on-device slot has h->serverId == sh["id"]: id match.
  //     Update name in place (rename-safe) and xpValue.  streak/doneToday
  //     are untouched — this is the fix for rename-resets-streak.
  //   • Else if names match (truncation-aware strncmp): name fallback.
  //     Adopt the server id so future syncs skip the name compare.
  //   • Else: new habit; add it with the server id attached.
  //
  // Pass 2: remove any on-device habit whose server id (or name for legacy
  //   serverId==-1 habits) is absent from the server list.
  //
  // IMPORTANT: all name compares use strncmp(a, b, HABIT_NAME_LEN - 1).
  // On-device names truncate at HABIT_NAME_LEN-1 (23) chars; a plain strcmp
  // against a longer dashboard name never matches.

  JsonArray serverHabits = respDoc["habits"].as<JsonArray>();

  // Pass 1 — upsert
  for (JsonObject sh : serverHabits) {
    const char* shName = sh["name"];
    int         shXp   = sh["xpValue"] | 0;
    int         shId   = sh["id"] | -1;

    bool found = false;

    // Step 1a: id match
    if (shId >= 0) {
      for (int i = 0; i < MAX_HABITS; i++) {
        Habit* h = tracker->get(i);
        if (!h->active || h->serverId != shId) continue;
        // Rename-safe: update name in-place, preserve streak + doneToday
        strncpy(h->name, shName, HABIT_NAME_LEN - 1);
        h->name[HABIT_NAME_LEN - 1] = '\0';
        h->xpValue = shXp;
        found = true;
        break;
      }
    }

    // Step 1b: name fallback (first sync, or legacy save with serverId==-1)
    if (!found) {
      for (int i = 0; i < MAX_HABITS; i++) {
        Habit* h = tracker->get(i);
        if (!h->active) continue;
        if (strncmp(h->name, shName, HABIT_NAME_LEN - 1) != 0) continue;
        h->serverId = shId;  // adopt server id — id match on all future syncs
        h->xpValue  = shXp;
        found = true;
        break;
      }
    }

    if (!found) {
      tracker->addHabit(shName, shXp, shId);
    }
  }

  // Pass 2 — remove habits no longer on server
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (!h->active) continue;

    bool stillExists = false;
    if (h->serverId >= 0) {
      // Id-based check
      for (JsonObject sh : serverHabits) {
        if ((sh["id"] | -1) == h->serverId) { stillExists = true; break; }
      }
    } else {
      // Name-based fallback for habits with no server id yet
      for (JsonObject sh : serverHabits) {
        if (strncmp(h->name, (const char*)sh["name"], HABIT_NAME_LEN - 1) == 0) {
          stillExists = true; break;
        }
      }
    }

    if (!stillExists) tracker->removeHabit(i);
  }

  // Apply XP awarded on the dashboard (quests, manual completions).
  // Only the delta above the last-applied total is credited — idempotent.
  pet->applyDashboardXpTotal(respDoc["dashXpTotal"] | 0);

  // Consume dashboard settings through pet's validated setter.
  // Sentinel -1 means the key was absent in the response — leave that field
  // unchanged (applySettings() rejects negatives for gain/decay, and < 0 for
  // hour, so passing -1 is a safe no-op for any field the server omitted).
  {
    PetSettings incoming = pet->getSettings();
    int gain  = respDoc["settings"]["moodGainPerHabit"] | -1;
    int decay = respDoc["settings"]["moodDecayPerMiss"]  | -1;
    int rHour = respDoc["settings"]["dailyResetHour"]    | -1;
    if (gain  >= 0) incoming.moodGainPerHabit = gain;
    if (decay >= 0) incoming.moodDecayPerMiss = decay;
    if (rHour >= 0) incoming.dailyResetHour   = rHour;
    pet->applySettings(incoming);
  }

  // Keep config version in sync so checkConfig() doesn't re-trigger immediately.
  lastKnownConfigVersion = respDoc["configVersion"] | lastKnownConfigVersion;

  return true;
}

bool WifiSync::checkConfig(Pet* pet, HabitTracker* tracker) {
  if (!isConnected()) return false;

  HTTPClient http;
  http.begin(serverUrl + "/api/config-version");
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, body)) return false;

  int serverVersion = doc["version"] | 0;
  if (serverVersion <= lastKnownConfigVersion) return false;

  Serial.printf("Config version advanced %d → %d, syncing now\n",
                lastKnownConfigVersion, serverVersion);
  lastKnownConfigVersion = serverVersion;
  return sync(pet, tracker);
}
