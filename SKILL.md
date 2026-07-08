---
name: cyberpet
description: >
  Conventions and architecture for the CyberPet project — a habit-tracking
  virtual pet for the Waveshare ESP32-S3-Touch-AMOLED-1.43C, plus its Docker
  web dashboard. Read this before creating, editing, or debugging any file in
  this project so changes stay consistent with the existing architecture.
  Triggers: any work on the ESP32 firmware (pet/habits/storage/ui/wifi_sync,
  CyberPet.ino), the dashboard (Express server, store.js, the HTML/JS control
  panel), the device<->dashboard sync protocol, or porting to another
  Waveshare AMOLED board.
---

# CyberPet project skill

## What this project is

Two independent deliverables that talk over HTTP:

- **`CyberPet/`** — ESP32 firmware. A virtual pet (a blob that evolves through
  XP stages) driven by completing habits. LVGL touch UI on a 466×466 round
  AMOLED. Runs fully standalone; optionally syncs with the dashboard.
- **`CyberPetDashboard/`** — Node/Express + static HTML control panel, shipped
  as a Docker image. Manages which habits/goals/settings exist. JSON-file
  storage behind a `get`/`update` seam.

Source of truth is split deliberately: the **device** owns streaks and
completion state; the **dashboard** owns the habit/goal/settings *definitions*.
Sync reconciles the two.

## Firmware architecture — respect this split

Logic files are **hardware-agnostic** and contain zero pin references:
`pet.h/.cpp`, `habits.h/.cpp`, `storage.h/.cpp`, `ui.h/.cpp`,
`wifi_sync.h/.cpp`. Keep them that way — it's what makes board swaps
(e.g. to the 1.75") a drop-in.

All board-specific code lives at exactly **two integration points** in
`CyberPet.ino`, both marked with `// TODO`:
1. display + touch + LVGL init in `setup()`
2. `lv_timer_handler()` / tick calls in `loop()`

These come from Waveshare's own example project for the board (the
`02_Example` folder in their repo), because pin mappings and driver init
sequences are board-specific. Never hardcode guessed pin numbers — point the
user to their board's example instead.

## Hard rules (violating these reintroduces fixed bugs)

- **Sync reconciliation is id-first, name-fallback.**
  1. Match by `Habit::serverId == server habit.id` — rename-safe, streak-safe.
  2. If no id match (serverId == -1 or first sync): fall back to
     truncation-aware `strncmp` name compare and *adopt* the server id so future
     syncs use path (1).
  Both paths must remain in `wifi_sync.cpp::sync()`. Removing the name-fallback
  path breaks first sync after a firmware flash (no ids on device yet). Removing
  the id path reverts the rename-resets-streak bug.
- **All habit-name comparisons must be truncation-aware:** use
  `strncmp(a, b, HABIT_NAME_LEN - 1)`, never plain `strcmp`. On-device names
  truncate at `HABIT_NAME_LEN-1` (23) chars; a plain `strcmp` against a longer
  dashboard name never matches, causing the habit to be removed and re-added
  (streak wiped) every sync cycle. This rule applies to the name-fallback path
  above, deletion checks for serverId==-1 habits, and any future name lookup.
- **After restoring habits from storage, call `HabitTracker::recount()`.**
  `habitCount` is not part of the persisted byte blob; skipping the recount
  makes `count()` return 0 and duplicate default habits get appended on boot.
- **Guard NVS writes with change detection.** Only write pet/habit state to
  flash when it actually differs from what was last written (see the `memcmp`
  guards in `loop()`). Don't rewrite identical bytes on a timer.
- **On the dashboard, `/api/settings` must whitelist known keys and drop
  null/empty/non-finite values.** A half-filled form must not overwrite valid
  settings. The frontend also strips empty fields before POSTing — keep both
  layers.
- **Server-side matching against device-supplied habit names must also be
  truncation-aware.** Use `namesMatch(a, b)` (defined near `stageFromXP` in
  `server.js`) for any comparison involving a name that may have originated on
  the device. Direct `===` against a 23-char-truncated name misses, which logs
  completions with `habitId: null` / `xpValue: 0` and zeros out streaks.

## Version assumptions

- **LVGL v8.2+.** `lv_obj_set_user_data`/`lv_obj_get_user_data` (used in
  `ui.cpp` to stash the habit index on list buttons) require ≥8.2. LVGL v9
  needs mechanical renames (`lv_btn_*` → `lv_button_*`, some style signatures).
  Always check which version the board's example ships before compiling.
- **ArduinoJson v6** (`StaticJsonDocument`). On v7 rename to `JsonDocument`.

## Dashboard extension point

`store.js` exposes `get()`, `update(mutatorFn)`, and `appendCompletions(d, names, date, deviceId)`.
This is the seam: to add persistence features or swap to a different database,
change `store.js` only — server routes never touch storage directly.
New API routes go in `server.js`; keep the existing REST shape
(`/api/habits`, `/api/goals`, `/api/settings`, `/api/pet`, `/api/sync`,
`/api/history`, `/api/history/streaks`).

`appendCompletions` is called **inside** a `store.update()` callback (not
standalone) so the pet-state write and log append land in a single SQLite
transaction. It dedupes per habit per day (truncation-aware `namesMatch`),
stamps records with the server's local date (device clock may skew around
midnight), attaches `deviceId`, and prunes entries older than
`HISTORY_KEEP_DAYS` days. Dashboard-side completions (from
`/api/habits/:id/complete`) push directly to `d.completionLog` inside their
own `update()` call — `appendCompletions` is for device sync only.

## Sync protocol (device ↔ dashboard)

Device POSTs to `/api/sync`:
```json
{ "deviceId": "<mac>", "petState": {...},
  "completedHabits": [{"id": 3, "name": "Move body"}, ...] }
```
`completedHabits` entries are `{id, name}` objects where `id` is the
dashboard's `habit.id` (stored as `Habit::serverId` on-device). The server
normalizes both the new object format and the legacy string-name format for
backward compatibility.

Server responds with `{ habits, goals, settings, dashXpTotal, configVersion }`.
Each habit object includes its `id`. The device reconciles id-first (rename-
safe), falls back to truncation-aware name compare for first-sync/legacy slots,
and adopts the server id on a name-match so future syncs skip the name compare.

## Known gaps to be aware of (don't "rediscover" them as bugs)

- Daily reset is **wall-clock-based via NTP** when WiFi is configured; falls
  back to the original `millis()`/24 h uptime path when standalone.
  `dailyResetHour` is now consumed from dashboard settings (default: 4 AM).
  An external I2C RTC (PCF85063/DS3231) can be added later by subclassing
  `TimeKeeper` and overriding `now()` — **do NOT write RTC driver code yet**,
  the I2C pads on the 1.43C are unverified.
- `timekeeping.cpp` is **firmware-only** (excluded from the sim build, same
  pattern as `storage.cpp`/`wifi_sync.cpp`). The sim 'd' key still calls
  `pet.dailyTick`/`habits.resetDaily` directly in `main_web.cpp`.
- Goals exist in the dashboard/API but aren't synced to the device yet.
- Renaming a habit on the dashboard now **preserves on-device streaks** —
  reconciliation is id-first (`Habit::serverId`), with truncation-aware name
  fallback for first-sync / legacy unsynced habits.
- Dashboard settings (`moodGainPerHabit`, `moodDecayPerMiss`, `dailyResetHour`)
  are now fully consumed. The sync response applies them via `pet.applySettings()`
  (validates ranges: 0-100 for mood fields, 0-23 for hour). `PetSettings` is
  persisted to NVS under key `"pet_settings"` with a size-guard; a standalone
  boot after a previous sync keeps the last-configured values. `WifiSync` no
  longer holds a `dailyResetHour` field — `loop()` reads it from
  `pet.getSettings().dailyResetHour`.

## Style

The user prefers terse, paste-ready outputs with code blocks, automation over
manual steps, and honest flagging of what's board-specific or untested rather
than confident guesses. When something depends on the board's example code or
a library version, say so explicitly instead of inventing pin numbers or APIs.

## Web simulator (`web_sim/`)

Runs the real firmware UI in a browser (Emscripten) or native SDL window.
Rules: firmware logic files compile against `web_sim/shim/Arduino.h` — if new
firmware code needs more Arduino API than that shim provides, keep it out of
the hardware-agnostic files. `storage.cpp`/`wifi_sync.cpp` are firmware-only
and never part of the sim build. LVGL is pinned to v8.3.11 in
`web_sim/CMakeLists.txt`; keep it matching whatever the device build uses.
Sim shortcuts: `d` = day rollover, `x` = +25 XP.
