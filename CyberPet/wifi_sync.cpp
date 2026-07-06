#include "wifi_sync.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // install via Library Manager: "ArduinoJson" by Benoit Blanchon
// NOTE: written against ArduinoJson v6 (StaticJsonDocument). In v7 the
// class is just JsonDocument - a two-line rename if you're on v7.

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

  // Build request body: device id, current pet state, which habits were
  // completed today (by dashboard-known name, since MVP has no shared IDs yet)
  StaticJsonDocument<1024> reqDoc;
  reqDoc["deviceId"] = WiFi.macAddress();

  PetState p = pet->getState();
  JsonObject petObj = reqDoc.createNestedObject("petState");
  petObj["xp"] = p.xp;
  petObj["stage"] = p.stage;
  petObj["mood"] = p.mood;
  petObj["daysAlive"] = p.daysAlive;
  petObj["hunger"] = p.hunger;
  petObj["alive"] = p.alive;

  // Send names, not slot indices - names are the shared key between device
  // and dashboard (the device doesn't know the dashboard's habit IDs).
  JsonArray completed = reqDoc.createNestedArray("completedHabits");
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (h->active && h->doneToday) completed.add(h->name);
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

  // Response contains { habits: [...], goals: [...], settings: {...} }
  StaticJsonDocument<2048> respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  if (err) {
    Serial.print("Sync response parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  // Reconcile habits: add any server-side habits not yet present on-device.
  // Simple name-match merge - see header comment for limitations.
  //
  // IMPORTANT: on-device names are truncated to HABIT_NAME_LEN-1 chars, so
  // all comparisons use strncmp with that same limit. A plain strcmp against
  // a longer dashboard name would never match, causing the habit to be
  // removed and re-added on every sync cycle (streaks lost, forever).
  JsonArray serverHabits = respDoc["habits"].as<JsonArray>();
  for (JsonObject sh : serverHabits) {
    const char* name = sh["name"];
    int xpValue = sh["xpValue"];

    bool found = false;
    for (int i = 0; i < MAX_HABITS; i++) {
      Habit* h = tracker->get(i);
      if (h->active && strncmp(h->name, name, HABIT_NAME_LEN - 1) == 0) {
        found = true;
        h->xpValue = xpValue; // pick up XP edits from dashboard
        break;
      }
    }
    if (!found) {
      tracker->addHabit(name, xpValue); // addHabit truncates to fit
    }
  }

  // Remove on-device habits no longer present on the server
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (!h->active) continue;
    bool stillExists = false;
    for (JsonObject sh : serverHabits) {
      if (strncmp(h->name, (const char*)sh["name"], HABIT_NAME_LEN - 1) == 0) {
        stillExists = true;
        break;
      }
    }
    if (!stillExists) tracker->removeHabit(i);
  }

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

  // Config has changed — do a full sync to pick up the new settings/habits.
  Serial.printf("Config version advanced %d → %d, syncing now\n",
                lastKnownConfigVersion, serverVersion);
  lastKnownConfigVersion = serverVersion;
  return sync(pet, tracker);
}
