#pragma once

// Over-the-air dictionary update (July 2026): when the dashboard's
// dictPushToken advances (tools/push_dict.py -> POST /api/dict/publish),
// the sketch calls dictUpdateRun() to pull fresh words.idx + defs.dat over
// WiFi HTTP and swap them in on the SD card — no more pulling the TF card.
//
// Firmware-only (WiFi + SD_MMC + HTTPClient), excluded from the web_sim
// build exactly like dict_backend_sd.cpp / wifi_sync.cpp.
//
// serverUrl: resolved dashboard base URL (wifiSync.getServerUrl()).
// BLOCKING for the whole download (~30-60 s for 36 MB) — the caller puts up
// a status screen first and keeps LVGL untouched until this returns.
// Each file lands in /dict/<name>.new, is verified against the manifest's
// size + MD5, and only then replaces the live file, so a failed or
// interrupted download never corrupts the current dictionary.
// Returns true when every file verified and swapped.
bool dictUpdateRun(const char* serverUrl);
