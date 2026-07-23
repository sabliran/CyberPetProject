---
name: cyberpet
description: >
  Conventions and architecture for the CyberPet project — a habit-tracking
  virtual pet for the Waveshare ESP32-S3-Touch-AMOLED-1.75 (plain variant,
  not -C, not -G), plus its Docker web dashboard. Read this before creating,
  editing, or debugging any file in this project so changes stay consistent
  with the existing architecture.
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
(e.g. back to the 1.43C — see the firmware README's porting notes) a
drop-in.

All board-specific code lives at exactly **two integration points** in
`CyberPet.ino`, both marked with `// TODO`:
1. display + touch + LVGL init in `setup()`
2. `lv_timer_handler()` / tick calls in `loop()`

These come from Waveshare's own example project for the **target board**
(for the 1.75: `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75` —
verify the example folder layout against that repo; it does not mirror the
1.43C's numbered 01–05 structure). Pin numbers and driver init sequences
always come from the target board's own Waveshare example, never from docs
or memory — never hardcode guessed GPIOs; point the user to their board's
example instead. Don't name a specific touch controller for the 1.75 in
docs or comments (sources conflict); say "capacitive touch (I2C), driver
from Waveshare's example".

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
  **Known mismatch to reconcile at integration time:** `web_sim` pins LVGL
  v8.3.11, while the 1.75 repo's Arduino example bundles v8.4.0 per its
  `libraries/lvgl/library.json` (read from GitHub July 2026 — can't be
  compile-verified off-device). Both are v8.x so the ≥8.2 assumptions hold,
  but pick one and align sim + device when integrating; don't guess.
- **ArduinoJson v7+** (`JsonDocument`). On v6, rename back to
  `StaticJsonDocument<N>` / `DynamicJsonDocument(N)`.

## Dashboard extension point

`store.js` exposes `get()`, `update(mutatorFn)`, and `appendCompletions(d, names, date, deviceId)`.
This is the seam: to add persistence features or swap to a different database,
change `store.js` only — server routes never touch storage directly.
New API routes go in `server.js`; keep the existing REST shape
(`/api/habits` incl. `/:id/complete`, `/api/goals`, `/api/quests`,
`/api/todos`, `/api/devnotes`, `/api/settings`, `/api/pet`, `/api/sync`,
`/api/config-version`, `/api/events` (SSE), `/api/history`,
`/api/history/streaks`, `/api/export`, `/api/storage`).

`appendCompletions` is called **inside** a `store.update()` callback (not
standalone) so the pet-state write and log append land in a single SQLite
transaction. It dedupes per habit per day (truncation-aware `namesMatch`),
stamps records with the server's local date (device clock may skew around
midnight), attaches `deviceId`, and prunes entries older than
`HISTORY_KEEP_DAYS` days. Dashboard-side completions (from
`/api/habits/:id/complete`) push directly to `d.completionLog` inside their
own `update()` call — `appendCompletions` is for device sync only.

## Sync protocol (device ↔ dashboard)

### USB bridge (preferred transport when plugged in)
`WifiSync::sync()` is transport-split: `buildSyncRequest()` /
`applySyncResponse()` are shared, and the transport is chosen per attempt.
If a host has the USB CDC port open (`usbAvailable()`), the device first
sends `SYNC <request-json>\n` over serial and waits 2 s for
`SYNCRESP <response-json>\n`; on timeout it falls back to WiFi HTTP.
`CyberPetDashboard/usb-bridge.py` (stdlib-only Python) is the host side:
it relays SYNC lines to POST /api/sync and echoes all other device log
lines, doubling as the serial monitor — never run `cat /dev/ttyACM0`
alongside it (two readers split the stream).
The bridge also runs INSIDE the dashboard container: it AUTO-STARTS with
the server (safe with nothing docked — the bridge retries the port every
2 s) and can be stopped/restarted from the dashboard topbar button or
`/api/bridge/start|stop|status` (server.js
spawns `python3 usb-bridge.py`; status returns the last log lines). The
compose file passes `/dev` through with a cgroup rule for char major 166
(ttyACM*), so it hot-plugs and needs no host chmod. Only ONE bridge may
run at a time — don't start the container bridge while a terminal
`petbridge` is attached to the same port. The full sync runs when
WiFi is configured OR USB is attached, so a USB-only setup syncs with no
WiFi credentials at all; the interval is 10 s while USB is attached
(SYNC_INTERVAL_USB_MS), 60 s otherwise. Firmware must call `Serial.setRxBufferSize(4096)`
BEFORE `Serial.begin()` — the response burst overflows the 256 B HWCDC
default.

### Dashboard addressing
DASHBOARD_URL may use a `*.local` mDNS hostname (e.g.
`http://omarchy.local:8090`) instead of a LAN IP: `WifiSync::resolveServerUrl()`
resolves it via ESPmDNS once after WiFi connects and re-resolves after a
connection-level HTTP failure (DHCP lease moved). Non-.local URLs pass
through untouched. The host must be running avahi/mDNS.

### Push config via SSE (primary path)
At boot the device opens a persistent `GET /api/events` SSE connection
(`WifiSync::beginEventStream()`). The server sends `event: config\ndata:
{"version":N}\n\n` immediately after every dashboard edit. The device reads
this non-blocking in every `loop()` tick (`pollEventStream()`) and calls
`sync()` the moment the event arrives — latency is typically <1 s.

If the stream drops (server restart, network hiccup), `pollEventStream()` sets
`sseConnected = false`; the loop retries `beginEventStream()` every 5 s and
falls back to the 5 s `checkConfig` poll in the meantime. The 60 s full sync
remains as the final safety net.

### Full sync (every 60 s, or triggered by push/poll)
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

- Daily reset uses the **PCF85063 onboard RTC** (I2C addr 0x51, fixed by
  spec) when available, NTP as a secondary source, and `millis()`/24 h
  uptime only as last resort (RTC stale + no WiFi = first ever boot).
  `RtcTimeKeeper` subclasses `TimeKeeper` and overrides `now()`; the RTC
  is set from NTP once per session and retains time across power cycles.
  `dailyResetHour` is consumed from dashboard settings (default: 4 AM).
  Wire (I2C bus) is started by Waveshare's board init before `initRtc()`
  is called — never add `Wire.begin()` in the firmware files; leave that
  to the Waveshare example's init block in `CyberPet.ino`.
- `timekeeping.cpp` is **firmware-only** (excluded from the sim build, same
  pattern as `storage.cpp`/`wifi_sync.cpp`). The sim 'd' key still calls
  `pet.dailyTick`/`habits.resetDaily` directly in `main_web.cpp`.
- Quests AND goals are synced (read-only): the sync response's `quests` and
  `goals` arrays are parsed into `WifiSync::quests`/`goals` and displayed on
  the device's quest screen (pet screen swipe-right) and goal screen (quest
  screen swipe-right — the carousel is pet > quests > goals, swipe left to
  walk back). Completing a quest/goal happens on the dashboard. Both arrays
  use the API's camelCase field names (`xpValue`), never the SQL column names.
  Both lists are cached in NVS (`Storage::saveQuests`/`saveGoals`, change-
  guarded) on every successful sync and restored at boot, so they survive
  reboots instead of vanishing until the next sync.
- Manual sync: press & hold on the pet screen (LVGL long-press, ~400 ms;
  blob events bubble so holding the blob works too) sets a flag consumed by loop()
  (`ui.consumeSyncRequest()`), which runs `sync()` and reports back via
  `ui.syncFinished(ok)` — a spinner overlay animates meanwhile (LVGL runs in
  a FreeRTOS task on-device, so it animates through the blocking HTTP call).
- Tapping a done habit on the device un-does it (streak decrement + exact
  XP/mood revert via `Pet::removeXP`). But there is no un-complete in the
  sync API: if a sync already ran, the dashboard's completion-log entry for
  that day remains (dedupe means a re-complete won't double-log either).
- LVGL v8 style transforms (`transform_zoom`/`transform_angle`) crash the
  real device's render path on plain objects — they're image-only in v8.
  Animate width/height instead (see the blob's landing squash / pat puff).
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
Sim shortcuts: `d` = day rollover, `x` = +25 XP, `m` = dev menu,
`space` = workout rep, `s` = sedentary nudge, `b` = battery cycle (100% → 10% → 60% charging),
`p` = Pomodoro guilt-trip, `a` = apps menu (stands in for the physical
button). Press & hold on the pet screen = manual sync
(sim fakes a success after 1.2 s); a quick click on the blob = pat/love
reaction; mouse-drag = swipe gestures.

## Apps menu

`PetUI::showAppsMenu()` is a launcher screen listing the built-in apps
(walk, sleep, trophies) — table-driven via `APP_ENTRIES[]` in `ui.cpp`, so a
future app is one table row plus one dispatch case in `appsBtnCB`.

Trophies are computed server-side (`computeTrophies` in server.js — pure
function of completionLog/stepHistory/sleepHistory/petState, nothing
persisted) and follow the quests/goals device contract exactly: earned names
ride the sync response, `WifiSync::getTrophies` → `ui.setTrophies` →
NVS-cached via `Storage::saveTrophies` for reboot survival. Focus joined
the menu in July 2026 when the dictionary app took over the pet screen's
swipe-down slot (dict_ui.cpp). The dictionary follows the push-up/squat
BOOT contract: NO gesture handlers on its screens — the physical BOOT
button is the only exit (sketch checks `dictScreenActive()` before its
apps-menu fallback; sim 'a' mirrors it), internal back buttons only walk
its own screens. Settings keeps its pet-screen swipe (up).

Dictionary data updates ship over WiFi — never pull the TF card. After
`tools/make_dict.py` regenerates `tools/dict_out/`, either use the
dashboard's Options → "SD card files" panel (upload both files, then "Send
to device" — GET `/api/dict/files` drives its listing/status chips) or run
`python3 tools/push_dict.py`: both upload the files to the dashboard
(`/api/dict/files/*`, stored in the container's data volume outside
store.js like motionlogs) and `POST /api/dict/publish` stamps a size+MD5
manifest and bumps the store's `dictPushToken`. The device applies it via
the petResetToken pattern: token rides every sync response, the sketch
compares against NVS (`Storage::loadDictToken`) and calls
`dictUpdateRun()` (dict_update.cpp, firmware-only) — blocking ~3 min
"Updating dictionary" screen while it streams to `/dict/*.new`, verifies
size+MD5, then swaps. Failures leave the live files untouched (token only
persists on success, 3 attempts per boot cap). If dict_format.h constants
changed, flash the firmware BEFORE pushing (the record layout must match).

The sleep app asks "how did you sleep?" once per day (good/medium/bad →
`Pet::logSleep`: good +12 mood +8 hunger, medium +5/+3, bad −15 xp −10 mood
−8 hunger). The UI applies pet effects and fires `SleepLogCB`; the sketch
persists the daily gate (`SleepState` via `Storage::saveSleepState`) stamped
with the RTC date, restores it at boot with `setSleepLogged`, and re-arms it
each new calendar day from loop().

The walk app's "pocket mode" button fires `PocketModeCB` (board-owned:
1.75B blanks the panel + gates `touchReadCB` so fabric can't tap anything;
steps keep counting). A BOOT short press wakes it straight back into the
walk screen — the BOOT handler checks `pocketMode` before opening the apps
menu.

The walk app displays steps fed in via `PetUI::setSteps()`; counting lives in
the board sketch (1.75B: software peak detector on accel magnitude — the
QMI8658's silicon pedometer never counted on this board — with a 10-step
entry filter at walking cadence: isolated bumps never reach the total, and
the warm-up burst is credited retroactively once walking is established.
Deltas accumulate into a `StepState` persisted via `Storage::saveStepState`). Daily reset compares the RTC calendar
date stamped into StepState; reaching `WALK_DAILY_GOAL` awards
`WALK_GOAL_XP` once per day (the `rewarded` flag survives reboots). It toggles: the
same call closes the menu if it's already showing, and any swipe dismisses
back to the pet. Board wiring: short press (50 ms–2 s) of BOOT/GPIO0 on both
boards — on the 1.75B a 2 s+ hold still powers off via the PMU, and on the
1.43C the `ui.showAppsMenu()` call must stay inside
`bsp_display_lock`/`unlock`.

## Device lv_conf.h (outside the repo!)

The 1.75B build depends on `~/Arduino/libraries/lv_conf.h` settings that are
NOT version-controlled here. On a fresh machine these must be set or bugs
resurface:
- `LV_MEM_SIZE (128U * 1024U)` with the pool in PSRAM:
  `LV_MEM_POOL_INCLUDE <esp32-hal-psram.h>` + `LV_MEM_POOL_ALLOC ps_malloc`
  (inside the `LV_MEM_ADR == 0` branch, replacing the `#undef`s). History:
  48K wedged at stage evolution, a 64K INTERNAL pool wedged (abort-reboot)
  building the dictionary's 50-row word list, and 96K internal starved the
  WiFi heap — PSRAM is the only placement that fits both. Draw buffers stay
  internal DMA (the sketch allocates those itself).
- `LV_ASSERT_HANDLER abort();` — the stock `while(1);` is a silent freeze
  with no serial output and no reboot; abort() panics loudly.
- Fonts enabled: montserrat 14, 20 (default), 32.
- `LV_COLOR_16_SWAP 1` (CO5300 QSPI byte order on the 1.75B).
