#include "wifi_sync.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>   // resolves *.local dashboard hostnames
#include <ArduinoJson.h> // install via Library Manager: "ArduinoJson" by Benoit Blanchon
// Requires ArduinoJson v7+ (JsonDocument). On v6, rename JsonDocument back
// to StaticJsonDocument<N> / DynamicJsonDocument(N) — a two-line change.
//
// ⚠  Cannot be compile-tested without the ESP32 toolchain.

void WifiSync::begin(const char* ssid, const char* password, const char* server) {
  ssid_     = String(ssid);
  password_ = String(password);
  serverUrlConfigured = String(server);
  serverUrl = serverUrlConfigured;

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
    resolveServerUrl();
  } else {
    Serial.println("WiFi connect failed - device will keep running standalone.");
  }
}

void WifiSync::kickReconnect() {
  if (ssid_.length() == 0) return;   // begin() never ran
  WiFi.disconnect();
  WiFi.begin(ssid_.c_str(), password_.c_str());
}

// Resolve a "*.local" dashboard host via mDNS so DASHBOARD_URL can be a
// stable hostname (e.g. http://omarchy.local:8090) instead of a DHCP IP
// that changes. Non-.local URLs pass through untouched.
void WifiSync::resolveServerUrl() {
  String url = serverUrlConfigured;
  String scheme = "http://";
  if (url.startsWith("http://"))       url.remove(0, 7);
  else if (url.startsWith("https://")) { scheme = "https://"; url.remove(0, 8); }
  int colon = url.lastIndexOf(':');
  String host = (colon > 0) ? url.substring(0, colon) : url;
  String port = (colon > 0) ? url.substring(colon)    : "";  // includes the ':'

  if (!host.endsWith(".local")) { serverUrl = serverUrlConfigured; return; }

  if (!mdnsStarted) {
    // Our own mDNS instance name is irrelevant; begin() is required before queries.
    // "cyberpet-device", NOT "cyberpet": the dashboard host publishes
    // cyberpet.local for the browser-facing panel, and mDNS names must be
    // unique per network.
    mdnsStarted = MDNS.begin("cyberpet-device");
    if (!mdnsStarted) { Serial.println("mDNS: init failed"); return; }
  }
  String name = host.substring(0, host.length() - 6);  // strip ".local"
  IPAddress ip = MDNS.queryHost(name);                 // blocking, ~2 s on miss
  if (ip == IPAddress()) {
    Serial.println("mDNS: could not resolve " + host + " (keeping previous URL)");
    return;  // keep whatever serverUrl already holds
  }
  serverUrl = scheme + ip.toString() + port;
  Serial.println("mDNS: " + host + " -> " + ip.toString());
}

bool WifiSync::isConnected() {
  connected = (WiFi.status() == WL_CONNECTED);
  return connected;
}

// Build the /api/sync request body.
// completedHabits: array of {id, name} objects.  id is the dashboard's
// habit.id (Habit::serverId); -1 when the habit has never been synced.
// The server uses id for accurate log attribution; name is kept alongside
// so old-format servers (string-only) can still parse the completion.
void WifiSync::buildSyncRequest(Pet* pet, HabitTracker* tracker, String& out) {
  JsonDocument reqDoc;
  reqDoc["deviceId"] = WiFi.macAddress();

  PetState p = pet->getState();
  JsonObject petObj = reqDoc["petState"].to<JsonObject>();
  petObj["xp"]       = p.xp;
  petObj["stage"]    = p.stage;
  petObj["mood"]     = p.mood;
  petObj["daysAlive"]= p.daysAlive;
  petObj["hunger"]   = p.hunger;
  petObj["alive"]    = p.alive;

  // Walking analytics: the server keys history by the device's own calendar
  // date when valid (year >= 2020), else falls back to its own date.
  JsonObject stepsObj = reqDoc["steps"].to<JsonObject>();
  stepsObj["today"]     = stepsToday_;
  stepsObj["year"]      = stepYear_;
  stepsObj["dayOfYear"] = stepDoy_;

  // Lifetime completed workout sessions (server keeps max; trophies/panels).
  reqDoc["backSessions"] = backSessions_;
  reqDoc["pushSessions"] = pushSessions_;
  reqDoc["focusSessions"] = focusSessions_;

  // Sleep rating rides along once logged; keyed server-side by its own date.
  if (sleepQuality_ >= 0) {
    JsonObject sleepObj = reqDoc["sleep"].to<JsonObject>();
    sleepObj["quality"]   = sleepQuality_;
    sleepObj["year"]      = sleepYear_;
    sleepObj["dayOfYear"] = sleepDoy_;
  }

  JsonArray completed = reqDoc["completedHabits"].to<JsonArray>();
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (h->active && h->doneToday) {
      JsonObject entry = completed.add<JsonObject>();
      entry["id"]   = h->serverId; // -1 when not yet matched to a server habit
      entry["name"] = h->name;
    }
  }

  serializeJson(reqDoc, out);
}

// One request/response round trip over the USB CDC serial bridge.
// Frame: device sends "SYNC <json>\n"; the host bridge (usb-bridge.py,
// which also acts as a serial monitor) POSTs it to /api/sync and writes
// back "SYNCRESP <json>\n". Ordinary Serial.print log lines don't collide:
// the bridge ignores lines without the SYNC prefix, and this reader ignores
// everything without the SYNCRESP prefix. Returns false if no bridge
// answers within 2 s — caller falls back to WiFi HTTP.
bool WifiSync::usbTransact(const String& body, String& resp) {
  while (Serial.available()) Serial.read();  // drop stale input

  Serial.print("SYNC ");
  Serial.println(body);

  uint32_t start = millis();
  String line;
  while (millis() - start < 2000) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n') {
        if (line.startsWith("SYNCRESP ")) {
          resp = line.substring(9);
          return true;
        }
        line = "";
      } else if (c != '\r') {
        line += c;
      }
    }
    delay(10);
  }
  return false;
}

bool WifiSync::sync(Pet* pet, HabitTracker* tracker) {
  String body;
  buildSyncRequest(pet, tracker, body);

  // USB-first: if a host has the CDC port open, prefer the serial bridge.
  // Works with no WiFi configured and bypasses LAN/firewall problems.
  if (usbAvailable()) {
    String usbResp;
    if (usbTransact(body, usbResp)) {
      if (applySyncResponse(usbResp, pet, tracker)) {
        Serial.println("Synced over USB bridge");
        return true;
      }
    }
    // No bridge answered (or bad response) — fall through to WiFi.
  }

  if (!isConnected()) return false;

  HTTPClient http;
  http.begin(serverUrl + "/api/sync");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(body);
  if (httpCode != 200) {
    Serial.printf("Sync failed, HTTP %d\n", httpCode);
    http.end();
    // Connection-level failure: the host's DHCP lease may have moved.
    // Re-resolve the .local name so the next attempt uses the fresh IP.
    if (httpCode <= 0) resolveServerUrl();
    return false;
  }

  String response = http.getString();
  http.end();

  return applySyncResponse(response, pet, tracker);
}

// Apply a /api/sync response (from either transport).
// Response contains { habits, goals, quests, settings, dashXpTotal, configVersion }.
bool WifiSync::applySyncResponse(const String& response, Pet* pet, HabitTracker* tracker) {
  JsonDocument respDoc;
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

  // Quests: dashboard-owned, read-only on device. Copy for the quest screen.
  questCount = 0;
  for (JsonObject q : respDoc["quests"].as<JsonArray>()) {
    if (questCount >= MAX_QUESTS) break;
    const char* qname = q["name"] | "";
    if (qname[0] == '\0') continue;
    strncpy(quests[questCount].name, qname, QUEST_NAME_LEN - 1);
    quests[questCount].name[QUEST_NAME_LEN - 1] = '\0';
    quests[questCount].xp = q["xpValue"] | 0;  // API shape is camelCase (server.js), not the SQL column name
    questCount++;
  }

  // Goals: dashboard-owned, read-only on device. Copy for the goal screen.
  goalCount = 0;
  for (JsonObject g : respDoc["goals"].as<JsonArray>()) {
    if (goalCount >= MAX_GOALS) break;
    const char* gname = g["name"] | "";
    if (gname[0] == '\0') continue;
    strncpy(goals[goalCount].name, gname, GOAL_NAME_LEN - 1);
    goals[goalCount].name[GOAL_NAME_LEN - 1] = '\0';
    goals[goalCount].xp = g["xpValue"] | 0;  // camelCase, same as quests
    const char* gperiod = g["period"] | "";
    strncpy(goals[goalCount].period, gperiod, GOAL_PERIOD_LEN - 1);
    goals[goalCount].period[GOAL_PERIOD_LEN - 1] = '\0';
    goalCount++;
  }

  // Trophies: earned names computed server-side. Copy for the trophy screen.
  trophyCount = 0;
  for (JsonVariant t : respDoc["trophies"].as<JsonArray>()) {
    if (trophyCount >= MAX_TROPHIES) break;
    const char* tname = t | "";
    if (tname[0] == '\0') continue;
    strncpy(trophies[trophyCount].name, tname, TROPHY_NAME_LEN - 1);
    trophies[trophyCount].name[TROPHY_NAME_LEN - 1] = '\0';
    trophyCount++;
  }

  // One-shot dashboard commands (see getPetResetToken in wifi_sync.h).
  petResetToken = respDoc["petResetToken"] | petResetToken;

  // Keep config version in sync so checkConfig() doesn't re-trigger immediately.
  lastKnownConfigVersion = respDoc["configVersion"] | lastKnownConfigVersion;

  return true;
}

bool WifiSync::beginEventStream() {
  if (!isConnected()) return false;

  // Parse host + port from serverUrl ("http://HOST:PORT")
  String url = serverUrl;
  if (url.startsWith("http://"))  url.remove(0, 7);
  else if (url.startsWith("https://")) url.remove(0, 8);
  int colon = url.lastIndexOf(':');
  String host   = (colon > 0) ? url.substring(0, colon)                : url;
  uint16_t port = (colon > 0) ? (uint16_t)url.substring(colon + 1).toInt() : 80;

  sseClient.stop();
  sseConnected = false;
  sseLineBuf   = "";

  if (!sseClient.connect(host.c_str(), port)) {
    Serial.println("SSE: TCP connect failed");
    return false;
  }

  // Send GET request
  sseClient.print("GET /api/events HTTP/1.1\r\n");
  sseClient.print("Host: " + host + "\r\n");
  sseClient.print("Accept: text/event-stream\r\n");
  sseClient.print("Cache-Control: no-cache\r\n");
  sseClient.print("Connection: keep-alive\r\n");
  sseClient.print("\r\n");

  // Skip HTTP response headers (wait up to 3 s)
  sseClient.setTimeout(3000);
  while (sseClient.connected()) {
    String line = sseClient.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;   // blank line = end of headers
  }
  sseClient.setTimeout(1000);   // restore sensible timeout for normal reads

  sseConnected = sseClient.connected();
  sseLastPing  = millis();
  Serial.println(sseConnected ? "SSE: stream connected" : "SSE: lost after headers");
  return sseConnected;
}

bool WifiSync::pollEventStream(Pet* pet, HabitTracker* tracker) {
  if (!sseConnected) return false;

  if (!sseClient.connected()) {
    Serial.println("SSE: stream disconnected");
    sseConnected = false;
    sseClient.stop();
    return false;
  }

  bool gotConfig = false;
  int avail = sseClient.available();
  while (avail-- > 0) {
    int c = sseClient.read();
    if (c < 0) break;
    if (c == '\n') {
      sseLineBuf.trim();
      // SSE lines: "event: config", "data: {...}", ": ping", chunk-size hex, empty
      if (sseLineBuf.startsWith("event:") && sseLineBuf.indexOf("config") >= 0) {
        gotConfig = true;
      }
      if (sseLineBuf.length() > 0) sseLastPing = millis();
      sseLineBuf = "";
    } else if (c != '\r') {
      sseLineBuf += (char)c;
    }
  }

  // Stale detection: server pings every 25 s; if silent for >60 s, reconnect
  if (millis() - sseLastPing > 60000) {
    Serial.println("SSE: stale (no ping), reconnecting");
    sseConnected = false;
    sseClient.stop();
    return false;
  }

  if (gotConfig) {
    Serial.println("SSE: config push received, syncing now");
    return sync(pet, tracker);
  }
  return false;
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

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  int serverVersion = doc["version"] | 0;
  if (serverVersion <= lastKnownConfigVersion) return false;

  Serial.printf("Config version advanced %d → %d, syncing now\n",
                lastKnownConfigVersion, serverVersion);
  lastKnownConfigVersion = serverVersion;
  return sync(pet, tracker);
}
