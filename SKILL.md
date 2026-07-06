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

- **Sync reconciliation is name-based, not ID-based.** The device doesn't know
  the dashboard's habit IDs; habit *names* are the shared key.
- **All habit-name comparisons must be truncation-aware:** use
  `strncmp(a, b, HABIT_NAME_LEN - 1)`, never plain `strcmp`. On-device names
  truncate at `HABIT_NAME_LEN-1` (23) chars; a plain `strcmp` against a longer
  dashboard name never matches, causing the habit to be removed and re-added
  (streak wiped) every sync cycle.
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

## Version assumptions

- **LVGL v8.2+.** `lv_obj_set_user_data`/`lv_obj_get_user_data` (used in
  `ui.cpp` to stash the habit index on list buttons) require ≥8.2. LVGL v9
  needs mechanical renames (`lv_btn_*` → `lv_button_*`, some style signatures).
  Always check which version the board's example ships before compiling.
- **ArduinoJson v6** (`StaticJsonDocument`). On v7 rename to `JsonDocument`.

## Dashboard extension point

`store.js` exposes `get()` and `update(mutatorFn)` over a single JSON file.
This is the seam: to add persistence features (completion history, analytics)
or swap to a real database, change `store.js` only — the server routes don't
touch storage directly. New API routes go in `server.js`; keep the existing
REST shape (`/api/habits`, `/api/goals`, `/api/settings`, `/api/pet`,
`/api/sync`).

## Sync protocol (device ↔ dashboard)

Device POSTs to `/api/sync`:
```json
{ "deviceId": "<mac>", "petState": {...}, "completedHabits": ["<name>", ...] }
```
Server responds with the current `{ habits, goals, settings }`. The device
then adds server habits it lacks, updates XP values, and removes habits no
longer on the server — all matched by truncation-aware name compare.

## Known gaps to be aware of (don't "rediscover" them as bugs)

- Daily reset is uptime-based (`millis()`), not wall-clock. The
  RTC-based fix (onboard PCF85063) is the top roadmap item; `dailyResetHour`
  already exists in settings waiting to be consumed.
- Goals exist in the dashboard/API but aren't synced to the device yet.
- Renaming a habit on the dashboard resets its on-device streak (name-based
  reconciliation). The proper fix is adding a `serverId` field to `Habit`.
- Dashboard settings (mood gain/decay, reset hour) aren't consumed by the
  firmware yet — they're plumbed but `pet.cpp` still hardcodes the values.

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
