# CyberPet

A habit-tracking virtual pet for the Waveshare ESP32-S3-Touch-AMOLED-1.43C.
Complete habits, earn XP, watch your blob evolve. Fully local — no license
key, no cloud dependency, runs standalone on-device.

## How it works

- **Pet screen** — shows your blob (color/size reflects evolution stage),
  current XP, mood bar. Tap anywhere to go to the habit list.
- **Habit screen** — list of habits with checkboxes. Tap one to mark it done
  for the day and award XP to your pet. Tap "Back to pet" to return.
- **Evolution stages** — Egg → Blob → Creature → Evolved, based on total XP.
- **Mood** — rises when you complete habits, decays if you skip a whole day.
  Doesn't currently affect evolution, just gives visual/emotional feedback —
  extend `pet.cpp` if you want mood to gate evolution or trigger regression.
- **Persistence** — pet + habit state is saved to the ESP32's flash (NVS) so
  it survives power loss and reboots.
- **Optional dashboard sync** — set your WiFi credentials and dashboard URL
  in `CyberPet.ino` and the device syncs with the CyberPetDashboard web
  control panel every 60s (habits/goals/settings down, pet state and
  completions up). Leave `WIFI_SSID` blank to run fully standalone.

## Files

| File | Purpose |
|---|---|
| `pet.h/.cpp` | Pet XP, evolution stage, mood logic |
| `habits.h/.cpp` | Habit list, streaks, daily completion tracking |
| `storage.h/.cpp` | Save/load pet + habit state to flash (NVS) |
| `ui.h/.cpp` | LVGL screens (pet view + habit list) |
| `wifi_sync.h/.cpp` | Optional sync client for the CyberPetDashboard API |
| `CyberPet.ino` | Integration skeleton — glues everything together |

## Setup

1. Get Waveshare's own example project for this board first:
   `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43C` → `02_Example`
   folder. This has the correct display/touch/QSPI driver init code for
   this exact board — don't skip this step, the pin mappings are specific
   to this hardware.
2. Copy `pet.h/.cpp`, `habits.h/.cpp`, `storage.h/.cpp`, `ui.h/.cpp` into
   that project's folder.
3. In their example's `setup()`, keep their display/touch init code, then
   add the CyberPet init calls (see the `// TODO` markers in `CyberPet.ino`
   for exactly where these plug in).
4. In their `loop()`, keep their `lv_timer_handler()` / tick calls, then add
   the CyberPet loop logic (daily check + periodic save).
5. Dependencies: `Preferences` ships built-in with the ESP32 Arduino core
   (nothing to install). If using dashboard sync, install **ArduinoJson**
   (by Benoit Blanchon) from the Library Manager.
6. Flash via USB-C from Arduino IDE (BOOT+RESET combo if it won't enter
   download mode automatically).

## Fixed bugs (v3 revision)

- **Dashboard XP was silently discarded** — XP from quests and manual habit
  completions was stored in `petState` on the dashboard, but the device
  overwrote `petState` on every sync. Fixed with a monotonic `dashXpTotal`
  counter in the server and `applyDashboardXpTotal()` on the device: only the
  delta above what was already applied is credited, so repeated calls are safe.
  Existing NVS saves are invalidated (the struct grew by one `int`) — the
  `sizeof` guard in `loadPet()` already handles this by falling back to defaults.
- **Long habit names produced null completions and zero streaks on the
  dashboard** — `recordCompletions` and `streakFor` in `server.js` used `===`
  for name matching, which never matched a name truncated to 23 chars on-device.
  Fixed with `namesMatch()`, consistent with the firmware's `strncmp` rule.
- **Sync response buffer overflow** — `StaticJsonDocument<2048>` in
  `wifi_sync.cpp` silently failed to parse responses that include habits +
  goals + quests + settings. Replaced with `DynamicJsonDocument(6144)`.

## Fixed bugs (v2 revision)

These were caught in a code audit — if you grabbed an earlier copy of these
files, re-download:

- **Duplicate habits after reboot** — `Storage::loadHabits` restored the
  habit array but never recalculated the internal count, so `count()`
  returned 0 and the default habits got appended again into free slots.
  Fixed with `HabitTracker::recount()`, called automatically on load.
- **Sync flapping with long habit names** — device names truncate at 23
  chars; comparing them with `strcmp` against a longer dashboard name never
  matched, so the habit was removed and re-added (streak reset) on *every*
  sync. All comparisons now use length-limited `strncmp`.
- **Sync sent meaningless IDs** — the device was sending its internal slot
  indices (0–7) as "completed habit IDs"; the dashboard has its own IDs.
  Now sends habit *names*, which are the actual shared key.
- **Flash wear** — state was rewritten to NVS every 5 seconds forever, even
  when unchanged. Now writes only happen when the state actually differs.
- Removed a declared-but-never-implemented method from `wifi_sync.h`.

## Known gaps / next steps

- **Daily reset uses uptime, not wall-clock time.** Right now a "day" is
  literally 24 hours of `millis()`, which drifts and resets at the wrong
  time if the device reboots. For a real midnight reset, read the onboard
  **PCF85063 RTC** each loop and compare `day-of-year` against
  `storage.loadLastResetDay()`. Get the RTC library from Waveshare's
  example repo (`01_Arduino_Libraries` folder).
- **LVGL version:** written against **LVGL v8.2+** —
  `lv_obj_set_user_data`/`lv_obj_get_user_data` (used in `ui.cpp`) don't
  exist before 8.2 (assign `obj->user_data` directly on 8.0/8.1). LVGL v9
  needs mechanical renames (`lv_btn_*` → `lv_button_*`, some style-call
  signatures). Check which version Waveshare's `02_Example` ships before
  compiling.
- **ArduinoJson version:** `wifi_sync.cpp` uses v6 syntax
  (`StaticJsonDocument`). On v7, rename to `JsonDocument` — two lines.
- **Renaming a habit on the dashboard** shows up on-device as remove+add
  (streak resets), since reconciliation is name-based. Fix: add a
  `serverId` field to the `Habit` struct and match on that.
- **Editing habits from the device itself** isn't wired up — only via the
  dashboard (or edit the defaults in `habits.cpp`).
- **Mood doesn't currently punish evolution** — pet only grows, never
  regresses. Add a check in `checkEvolution()` if you want low mood to be
  able to demote a stage.

## Future additions & development roadmap

Ideas collected during development, roughly ordered from quick wins to
bigger projects. The "Known gaps" section above covers *fixes*; this is
for *new stuff*.

### Quick wins (an evening each)

- **RTC-based midnight reset** — the single most impactful improvement.
  Use the onboard PCF85063 (library in Waveshare's `01_Arduino_Libraries`)
  so habits reset at your configured hour instead of "24h of uptime".
  The `dailyResetHour` setting already exists in the dashboard waiting
  to be consumed.
- **Pet idle animations** — LVGL animations on the blob (slow bounce,
  occasional blink via a shrinking ellipse overlay, mood-based wobble
  speed). Pure `lv_anim_t` work, no new hardware.
- **Streak display on pet screen** — show the longest current streak
  next to XP; it's already tracked per habit, just not surfaced.
- **Completion celebration** — brief particle/confetti effect (a few
  animated circles) when checking off a habit. Cheap dopamine, and it's
  what makes tamagotchi-style devices feel alive.
- **Consume dashboard settings** — wire the settings from the sync
  response into `Pet`/`HabitTracker` (mood gain/decay values, reset hour)
  so the dashboard settings panel actually controls the device.

### Medium projects

- **Server-ID based sync** — add a `serverId` field to `Habit`, match on
  that instead of names. Fixes the rename-resets-streak limitation and
  makes long names a non-issue.
- **Goals on-device** — weekly/monthly goals already exist in the
  dashboard/API; add a third LVGL screen for them with the RTC handling
  week/month rollovers.
- **Pet regression** — sustained low mood demotes a stage (evolved →
  creature). Makes neglect actually matter; the check belongs in
  `checkEvolution()`. Balance carefully — punishing too hard is how habit
  apps get abandoned.
- **Completion history + charts** — store dated completions in `store.js`
  instead of just the latest list, then add a heatmap/streak chart panel
  to the dashboard.
- **IMU interactions** — the onboard QMI8658 can detect pickup/shake:
  wake the screen on pickup, make the blob react (dizzy animation) on
  shake. Waveshare's examples include the IMU driver.
- **Battery status** — read charge level via the AXP2101 power chip and
  show a small indicator; warn when the pet is "getting sleepy" (low
  battery).

### Bigger ideas

- **n8n/self-hosted integration** — a webhook from the dashboard on
  completion events opens everything up: Discord notifications when a
  streak hits milestones, auto-logging to a database, evening reminders
  if nothing's checked off. The dashboard's `store.update` calls are the
  natural hook point.
- **Habitica bridge** — swap or augment `store.js` with Habitica's open
  API so device completions score XP in a real Habitica account
  (party/quest mechanics for free).
- **LVGL web simulator** — ✅ DONE, see `web_sim/`. Compiles `ui.cpp` + logic with Emscripten
  (lvgl/lv_web_emscripten) for a browser-based twin of the device screen;
  useful for iterating on UI without reflashing. Note this needs an
  SDL/Emscripten `setup()` path alongside the hardware one.
- **Voice interaction** — the 1.43C has an audio codec onboard (no mic
  array — that's the 1.75" board). A small I2S/PDM mic on the exposed
  header could enable "done!" voice check-offs via wake-word libs like
  ESP-SR, or by streaming to a local AI stack.
- **Multiple pets / pet types** — different starting eggs with different
  evolution lines and stage art; store the pet type in `PetState`.
- **Custom pixel-art sprites** — replace the circle blob with LVGL image
  assets per stage/mood. LVGL's image converter turns PNGs into C arrays.
- **On-device habit editing** — an LVGL keyboard screen, or the lighter
  path: a serial command interface (`addhabit:Name:XP` over USB).
- **A `SKILL.md` for Claude** — a project skill file that captures this
  codebase's conventions so future Claude sessions (or Claude Code) stay
  consistent instead of re-deriving everything. Good things to encode:
  the architecture split (hardware-agnostic logic files vs the two
  board-specific integration points in `CyberPet.ino`); that sync
  reconciliation is name-based and comparisons must stay truncation-aware
  (`strncmp` at `HABIT_NAME_LEN-1`); the LVGL v8.2+ / ArduinoJson v6
  version assumptions; that NVS writes should stay change-guarded; the
  `store.update`/`get` seam on the dashboard as the extension point; and
  the rule that new pet/habit logic stays hardware-agnostic so board swaps
  (e.g. to the 1.75") remain drop-in. Drop it at the repo root as
  `SKILL.md`, or under `.claude/skills/cyberpet/SKILL.md` if using Claude
  Code, and Claude reads it before touching the project. Cheapest way to
  keep a long-running hobby project coherent across many sessions.

### Ideas: activity & focus features

Ways to make the pet react to what your body is doing, not just what you
tap. All of these feed naturally into the existing XP/mood system.

- **Step counting** — the onboard QMI8658 IMU supports motion detection
  and step counting, but only if the device is *on you* (pocket/clipped),
  since a desk-bound pet can't count your steps. Two viable designs:
  carry mode (battery + a clip case, steps counted on-device, auto-completes
  a "Move body" habit at a threshold), or phone-as-sensor (your phone counts
  steps and posts them to the dashboard API, which awards XP on next sync —
  no hardware changes, an n8n flow or an iOS Shortcut hitting `/api/sync`
  would do it).
- **Movement / sedentary nudges** — the inverse of step counting and more
  desk-realistic: if the IMU sees zero vibration and no touch input for
  ~an hour while the screen is on, the pet gets visibly restless (droopy
  blob, "I want a walk" bubble) and mood ticks down slightly until it
  detects being picked up or moved. Uses the pickup/shake detection the
  QMI8658 already does well.
- **Posture reminders** — honest note: the device can't *measure* your
  posture (the IMU senses its own orientation, not your spine). What works
  instead: timed posture check-ins — every N minutes the pet does a
  "stretch" animation as a cue, and tapping it within a minute confirms
  you straightened up and earns a small XP drip. Cue + confirmation, not
  sensing. (True posture sensing would need a wearable IMU or a camera —
  a separate ESP32 + IMU clipped to a chair back is a fun follow-up
  project that could report to the same dashboard API.)
- **Pomodoro mode** — a natural fit for a round desk screen. Third LVGL
  screen with a ring-shaped countdown (the circular display is literally
  made for this): 25 min focus / 5 min break, pet visibly "concentrates"
  during focus blocks and celebrates on completion. Completed pomodoros
  award XP like any habit, and a "N pomodoros today" counter can feed a
  weekly goal. The IMU adds a nice touch: if the device gets picked up or
  moved mid-focus-block (i.e., you grabbed your phone... or the pet), it
  can guilt-trip you with a disappointed blob face.

### Porting notes (for future hardware)

The 1.75" AMOLED board (also 466×466, same CO5300/CST9217/QMI8658/
PCF85063/AXP2101 chips) is a near drop-in: only the display/touch init
block in `CyberPet.ino` changes, sourced from that board's own example
project since pin mappings differ. The 1.75 adds a dual mic array +
echo cancellation (better for voice features) and has a GPS variant
(1.75-G) that would suit location-based quests. All logic files
(`pet`, `habits`, `storage`, `wifi_sync`) are hardware-agnostic and
port unchanged.

## A note on the Waveshare code's license

The `waveshareteam/ESP32-S3-Touch-AMOLED-1.43C` repo (as of July 2026: 3
commits, v1.1.0 dated 2026-04-20, folders 01–05 incl. schematics and
structural designs) has **no LICENSE file**. Waveshare's *other* board repos
(e.g. the 1.8" AMOLED) are Apache-licensed, but strictly speaking unlicensed
public code defaults to all-rights-reserved. Using and modifying their
drivers/examples on their own hardware is clearly the intended use; just
don't assume you can redistribute their driver code in your own public repo
without checking with them. Everything in *this* project (pet/habits/
storage/ui/wifi_sync) is original and yours to license however you want.

## Editing default habits

Edit the `habits.init()` seed list in `habits.cpp`:

```cpp
addHabit("Drink water", 5);
addHabit("Move body", 10);
```

Name max length is 24 chars (`HABIT_NAME_LEN`), and up to 8 habits total
(`MAX_HABITS`) — bump these in `habits.h` if you want more, just mind flash
usage on saves.
