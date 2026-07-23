// See dict_update.h. Flow: GET /api/dict/manifest (sizes + MD5s), stream
// each file to /dict/<name>.new on the SD card with an incremental MD5,
// verify, then remove+rename over the live files. dictReset() runs first so
// no open handle survives the swap; the next dictionary open re-inits.
#include "dict_update.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <FS.h>
#include <SD_MMC.h>

#include "dict.h"

// One manifest file entry, streamed to /dict/<name>.new. Size and MD5 both
// checked before the caller swaps anything.
static bool downloadOne(const String& base, const char* name,
                        uint32_t size, const char* md5) {
  String tmp = String("/dict/") + name + ".new";
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(base + "/api/dict/files/" + name)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("dict update: %s HTTP %d\n", name, code);
    http.end();
    return false;
  }

  File f = SD_MMC.open(tmp, FILE_WRITE);
  if (!f) {
    Serial.printf("dict update: cannot create %s\n", tmp.c_str());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  MD5Builder sum;
  sum.begin();
  static uint8_t buf[4096];
  uint32_t got = 0, lastLog = 0, lastData = millis();
  while (got < size) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));
      if (n <= 0) break;
      if (f.write(buf, n) != (size_t)n) { got = 0; break; }  // card write failed
      sum.add(buf, n);
      got += n;
      lastData = millis();
      if (got - lastLog >= 4 * 1024 * 1024) {
        lastLog = got;
        Serial.printf("dict update: %s %lu/%lu KB\n", name,
                      (unsigned long)(got / 1024), (unsigned long)(size / 1024));
      }
    } else {
      if (!http.connected()) break;
      if (millis() - lastData > 15000) break;  // stalled stream
      delay(1);
    }
  }
  f.close();
  http.end();

  if (got != size) {
    Serial.printf("dict update: %s short/failed at %lu of %lu bytes\n",
                  name, (unsigned long)got, (unsigned long)size);
    SD_MMC.remove(tmp);
    return false;
  }
  sum.calculate();
  if (sum.toString() != md5) {
    Serial.printf("dict update: %s MD5 mismatch (got %s)\n",
                  name, sum.toString().c_str());
    SD_MMC.remove(tmp);
    return false;
  }
  Serial.printf("dict update: %s verified (%lu KB)\n", name,
                (unsigned long)(size / 1024));
  return true;
}

bool dictUpdateRun(const char* serverUrl) {
  if (!serverUrl || !*serverUrl) return false;
  String base = serverUrl;

  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(base + "/api/dict/manifest")) return false;
  int code = http.GET();
  String body = (code == 200) ? http.getString() : String();
  http.end();
  if (code != 200) {
    Serial.printf("dict update: manifest HTTP %d\n", code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["files"].size() == 0) {
    Serial.println("dict update: bad manifest");
    return false;
  }

  SD_MMC.mkdir("/dict");   // first-ever card: directory may not exist
  dictReset();             // release the live files before touching them

  // Download + verify everything before swapping anything: a half-failed
  // update leaves the current dictionary untouched.
  for (JsonObject fo : doc["files"].as<JsonArray>()) {
    const char* name = fo["name"] | "";
    uint32_t size    = fo["size"] | 0;
    const char* md5  = fo["md5"]  | "";
    if (!*name || size == 0 || strlen(md5) != 32 || strchr(name, '/')) {
      Serial.println("dict update: bad manifest entry");
      return false;
    }
    if (!downloadOne(base, name, size, md5)) return false;
  }

  for (JsonObject fo : doc["files"].as<JsonArray>()) {
    const char* name = fo["name"] | "";
    String live = String("/dict/") + name;
    SD_MMC.remove(live);  // FAT rename won't overwrite; ignore if absent
    if (!SD_MMC.rename(live + ".new", live)) {
      Serial.printf("dict update: rename %s failed\n", name);
      return false;
    }
  }
  Serial.println("dict update: complete");
  return true;
}
