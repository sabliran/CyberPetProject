# CyberPet

A habit-tracking virtual pet for the Waveshare ESP32-S3-Touch-AMOLED-1.75
(plain variant — not the -C, not the GPS-equipped -G).
Complete habits, earn XP, watch your blob evolve. Fully local — no license
key, no cloud dependency, runs standalone on-device.

## How it works

- **Pet screen** — shows your blob (color/size reflects evolution stage),
  current XP, mood bar, and hunger bar. Tap anywhere to go to the habit list.
- **Habit screen** — list of habits with checkboxes (each row shows its XP
  value and current streak). Tap one to mark it done for the day and award XP
  to your pet. Three navigation buttons at the bottom: Back, Workout, and
  Focus (Pomodoro).
- **Workout screen** — rep counter with difficulty levels; hitting the target
  feeds the pet. Meal size scales with difficulty (Easy +30 / Medium +55 /
  Hard +90 hunger, with matching mood boosts).
- **Hunger & death** — hunger drains continuously (~2/hour, ≈48/day), so
  roughly one Medium workout per day keeps the pet fed. At hunger 0, one more
  unfed day kills the pet (mood locked to 0, "DEAD" indicator). Low hunger
  also hard-caps mood.
- **XP & streak bonuses** — habit XP scales with the habit's streak: +25% at
  3 days, +50% at 7, +75% at 14, +100% at 30. Completing the last habit of
  the day pays a "perfect day" bonus (10 + 5×habit count XP). Level L costs
  50·L+50 XP (level 1 = 100 XP, each level 50 XP more), and every level adds
  a companion dot orbiting the blob.
- **Pomodoro screen** — a ring-shaped countdown on the round display: 25 min
  focus block / 5 min break, cycling up to 4 blocks per session. Each completed
  focus block awards +25 XP and shows a popup. If the device is picked up
  mid-focus (IMU), `pomodoroGuiltTrip()` flashes the arc red with "⚠ FOCUS!"
  for 1.5 s before reverting.
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
| `ui.h/.cpp` | LVGL screens (pet view, habit list, workout, Pomodoro) |
| `wifi_sync.h/.cpp` | Optional sync client for the CyberPetDashboard API |
| `timekeeping.h/.cpp` | Wall-clock time source (NTP); hardware-agnostic base class |
| `CyberPet.ino` | Integration skeleton — glues everything together |

## Setup

1. Get Waveshare's own example project for this board first:
   `github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75`. Verify the example
   folder layout against that repo (it does **not** mirror the 1.43C's
   numbered 01–05 structure; as of July 2026 the Arduino examples live under
   `examples/` — checked on GitHub, not locally). This has the correct
   display/touch/QSPI driver init code for this exact board — don't skip
   this step, the pin mappings are specific to this hardware.
2. Copy `pet.h/.cpp`, `habits.h/.cpp`, `storage.h/.cpp`, `ui.h/.cpp` into
   that project's folder.
3. In their example's `setup()`, keep their display/touch init code, then
   add the CyberPet init calls (see the `// TODO` markers in `CyberPet.ino`
   for exactly where these plug in).
4. In their `loop()`, keep their `lv_timer_handler()` / tick calls, then add
   the CyberPet loop logic (daily check + periodic save).
5. Dependencies: `Preferences` ships built-in with the ESP32 Arduino core
   (nothing to install). If using dashboard sync, install **ArduinoJson**
   (by Benoit Blanchon, **v7+**) from the Library Manager.
6. Flash via USB-C from Arduino IDE (BOOT+RESET combo if it won't enter
   download mode automatically).

## Fixed bugs (v5 revision)

- **Dashboard settings weren't consumed by the firmware** — `moodGainPerHabit`,
  `moodDecayPerMiss`, and `dailyResetHour` existed in the dashboard/API but
  `pet.cpp` still hardcoded mood gain (2) and decay (15). Fixed by adding a
  `PetSettings` struct to the hardware-agnostic layer (`pet.h`), with
  `DEFAULT_PET_SETTINGS = { 2, 15, 4 }` matching the former literals. The sync
  response now applies all three fields via `pet.applySettings()`, which
  validates each field before storing (0-100 for mood fields, 0-23 for hour) —
  a bad payload can't zero out mood mechanics. `PetSettings` is persisted to NVS
  under key `"pet_settings"` with a size-guard, so a standalone boot after a
  previous WiFi sync keeps the last-configured values instead of reverting to
  compile-time defaults. `WifiSync` no longer holds a `dailyResetHour` field —
  the reset hour is read from `pet.getSettings().dailyResetHour` in `loop()`.

## Fixed bugs (v4 revision)

- **Renaming a habit on the dashboard wiped its on-device streak** — the
  device matched habits by name; renaming on the dashboard meant the old name
  was never found, so the habit was removed and re-added (streak reset) on
  every sync. Fixed by adding `int serverId` to the `Habit` struct and
  switching reconciliation to id-first: the device matches `Habit::serverId`
  against the server's `habit.id` and updates the name in place. The name
  path stays as a fallback for the first sync (no ids on device yet) and
  adopts the server id on match so subsequent syncs use the id path.
  NVS habits key bumped to `habits_v2`; old saves are discarded cleanly
  (hardware not yet flashed — acceptable reset).

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

- **Daily reset now uses wall-clock time via NTP** when WiFi is configured.
  Set your POSIX `TIMEZONE` string in `CyberPet.ino` (e.g.
  `"EST5EDT,M3.2.0,M11.1.0"`) and the reset fires at `dailyResetHour` local
  time (synced from dashboard settings, default 4 AM).  Without WiFi the
  original 24 h `millis()` uptime path is used unchanged.
  The last-reset date is persisted to NVS so a reboot after midnight doesn't
  double-fire or skip the reset.
- **Onboard PCF85063 RTC not yet wired in — this is the top pending item**
  (implementation is a separate task). The 1.75 has a PCF85063 with a
  32.768 kHz crystal on the ESP32's I2C bus (per Waveshare's docs/schematic —
  not yet verified on hardware here), so no soldering is needed.
  `timekeeping.h` defines a virtual `TimeKeeper` base; adding the RTC backend
  is a `now()` override with no changes to pet/habits logic. Take the I2C
  init and pin setup from Waveshare's own example for this board — never
  hardcode GPIO numbers from docs.
- **LVGL version:** written against **LVGL v8.2+** —
  `lv_obj_set_user_data`/`lv_obj_get_user_data` (used in `ui.cpp`) don't
  exist before 8.2 (assign `obj->user_data` directly on 8.0/8.1). LVGL v9
  needs mechanical renames (`lv_btn_*` → `lv_button_*`, some style-call
  signatures). Check which version the 1.75 repo's Arduino example bundles
  before compiling — its `libraries/lvgl/library.json` says **8.4.0** as of
  July 2026 (read from GitHub, not compile-verified), while `web_sim` pins
  8.3.11; reconcile the two at integration time.
- **ArduinoJson version:** `wifi_sync.cpp` now uses v7 syntax (`JsonDocument`).
  Install **ArduinoJson v7+** from the Library Manager. On v6, rename
  `JsonDocument` back to `StaticJsonDocument<N>` / `DynamicJsonDocument(N)`.
- **Editing habits from the device itself** isn't wired up — only via the
  dashboard (or edit the defaults in `habits.cpp`).
- **Mood regression** — ✅ DONE. If `state.mood` drops below 20 (roughly
  6 consecutive missed-habit days with the default decay of 15/day), the
  displayed stage is demoted by one. Fully recoverable: raise mood above 20
  and the stage returns immediately on the next `dailyTick` or XP gain. XP
  is never touched. Threshold is `MOOD_REGRESSION_THRESHOLD` in `pet.h`.

## Future additions & development roadmap

Ideas collected during development, roughly ordered from quick wins to
bigger projects. The "Known gaps" section above covers *fixes*; this is
for *new stuff*.

### Quick wins (an evening each)

- **I2C RTC backend — top pending item** (implementation is a separate
  task). NTP covers the common WiFi case; for a fully offline wall-clock
  reset, wire in the onboard PCF85063 (with 32.768 kHz crystal, on the
  ESP32's I2C bus per the 1.75 schematic) as a `TimeKeeper` subclass.
  Driver/library and I2C init come from the 1.75 repo's Arduino examples —
  verify the folder layout there; don't assume the 1.43C repo's structure.
- **Pet idle animations** — ✅ DONE, see `ui.cpp`: per-stage bounce/wobble,
  blink timer, glow pulse, and roaming with squash-and-stretch landing.
- **Streak display on pet screen** — partially done: each habit-list row now
  shows its streak (`+Xxp streak N`). Still open: surface the longest
  current streak on the *pet* screen next to XP.
- **Completion celebration** — partially done: an XP popup animates on
  check-off (`showXpPopup` in `ui.cpp`). Still open: a particle/confetti
  burst (a few animated circles). Cheap dopamine, and it's what makes
  tamagotchi-style devices feel alive.
- **Consume dashboard settings** — ✅ DONE (v5). `moodGainPerHabit`,
  `moodDecayPerMiss`, and `dailyResetHour` are now applied from the sync
  response and persisted to NVS.

### Medium projects

- **Server-ID based sync** — ✅ DONE (v4). `Habit::serverId` + id-first
  reconciliation with truncation-aware name fallback; fixed the
  rename-resets-streak limitation.
- **Goals on-device** — weekly/monthly goals already exist in the
  dashboard/API; add a third LVGL screen for them with the RTC handling
  week/month rollovers.
- **Pet regression** — ✅ DONE. See Known gaps above for details.
- **Completion history + charts** — ✅ DONE. Dated completions live in the
  dashboard's `completion_log` table (365-day rolling window); the Overview
  tab shows a GitHub-style heatmap plus per-habit current/best streaks
  (`GET /api/history`, `GET /api/history/streaks`).
- **IMU interactions** — the 1.75's onboard QMI8658 6-axis IMU can detect
  pickup/shake: wake the screen on pickup, make the blob react (dizzy
  animation) on shake. No extra hardware needed; take the IMU driver from
  the 1.75 repo's examples.
- **Battery status** — the 1.75 has an AXP2101 PMIC and an MX1.25 3.7 V
  li-ion header with charge/discharge onboard: read charge level via the
  AXP2101 and show a small indicator; warn when the pet is "getting
  sleepy" (low battery). No extra hardware needed.

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
- **Voice interaction** — the 1.75 has an ES8311 codec, ES7210 echo
  cancellation, a dual digital mic array, and a speaker onboard (per
  Waveshare's docs), so no extra hardware is needed for "done!" voice
  check-offs via wake-word libs like ESP-SR, or for streaming to a local
  AI stack.
- **Location-based quests** — **requires the 1.75-G variant** (LC76G GPS
  module); the plain 1.75 targeted here only has an IPEX antenna holder.
- **Multiple pets / pet types** — different starting eggs with different
  evolution lines and stage art; store the pet type in `PetState`.
- **Custom pixel-art sprites** — replace the circle blob with LVGL image
  assets per stage/mood. LVGL's image converter turns PNGs into C arrays;
  for bigger sprite sets (or audio assets), the 1.75's onboard TF card
  slot is the natural future storage path.
- **On-device habit editing** — an LVGL keyboard screen, or the lighter
  path: a serial command interface (`addhabit:Name:XP` over USB).
- **A `SKILL.md` for Claude** — ✅ DONE, see `SKILL.md` at the repo root
  (plus `CLAUDE.md` pointing at it). A project skill file that captures this
  codebase's conventions so future Claude sessions (or Claude Code) stay
  consistent instead of re-deriving everything. Things it encodes:
  the architecture split (hardware-agnostic logic files vs the two
  board-specific integration points in `CyberPet.ino`); that sync
  reconciliation is name-based and comparisons must stay truncation-aware
  (`strncmp` at `HABIT_NAME_LEN-1`); the LVGL v8.2+ / ArduinoJson v7+
  version assumptions; that NVS writes should stay change-guarded; the
  `store.update`/`get` seam on the dashboard as the extension point; and
  the rule that new pet/habit logic stays hardware-agnostic so board swaps
  (e.g. back to the 1.43C) remain drop-in. Drop it at the repo root as
  `SKILL.md`, or under `.claude/skills/cyberpet/SKILL.md` if using Claude
  Code, and Claude reads it before touching the project. Cheapest way to
  keep a long-running hobby project coherent across many sessions.

### Ideas: activity & focus features

Ways to make the pet react to what your body is doing, not just what you
tap. All of these feed naturally into the existing XP/mood system.

- **Step counting** — the 1.75's onboard QMI8658 IMU supports motion
  detection and step counting (no add-on hardware needed), but only if the
  device is *on you* (pocket/clipped), since a desk-bound pet can't count
  your steps. Two viable designs: carry mode (a 3.7 V li-ion cell on the
  onboard MX1.25 header + a clip case, steps counted on-device,
  auto-completes a "Move body" habit at a threshold), or phone-as-sensor
  (your phone counts steps and posts them to the dashboard API, which
  awards XP on next sync — no hardware changes, an n8n flow or an iOS
  Shortcut hitting `/api/sync` would do it).
- **Movement / sedentary nudges** — the inverse of step counting and more
  desk-realistic: if the IMU sees zero vibration and no touch input for
  ~an hour while the screen is on, the pet gets visibly restless (droopy
  blob, "I want a walk" bubble) and mood ticks down slightly until it
  detects being picked up or moved. Uses the pickup/shake detection the
  onboard QMI8658 already does well — fully supported on the 1.75 with no
  extra hardware.
  **Done (UI / inactivity proxy):** `sedentaryCheckCB` polls
  `lv_disp_get_inactive_time()` every 60 s; after 1 h of no touch input
  the blob goes heavy-lidded and droopy (`sedentaryActive` flag in
  `applyMoodExpression()`), a "I want a stretch..." bubble floats up, and
  mood is knocked −5. Clears automatically within 60 s of the next touch.
  Wiring real IMU pickup detection replaces the LVGL inactivity proxy and
  is the next step once the QMI8658 driver is integrated.
- **Posture reminders** — honest note: the device can't *measure* your
  posture (the IMU senses its own orientation, not your spine). What works
  instead: timed posture check-ins — every N minutes the pet does a
  "stretch" animation as a cue, and tapping it within a minute confirms
  you straightened up and earns a small XP drip. Cue + confirmation, not
  sensing. (True posture sensing would need a wearable IMU or a camera —
  a separate ESP32 + IMU clipped to a chair back is a fun follow-up
  project that could report to the same dashboard API.)
- **Pomodoro mode** — ✅ DONE, see `ui.cpp`. Ring-shaped countdown on the
  round display, 25 min focus / 5 min break, up to 4 blocks per session,
  +25 XP per completed block with XP popup. IMU guilt-trip on pickup via
  `pomodoroGuiltTrip()` (red arc flash + "⚠ FOCUS!" for 1.5s) — the UI hook
  exists; wiring it to real pickup detection is now onboard-supported via
  the 1.75's QMI8658. Accessible from the Focus button on the habit screen.

### Porting notes: the 1.43C (the original target board)

This project originally targeted the Waveshare ESP32-S3-Touch-AMOLED-1.43C
before being retargeted to the 1.75. Porting back (or maintaining both) is
still a drop-in: only the display/touch init block in `CyberPet.ino`
changes, sourced from the 1.43C's own example repo
(`github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43C`, `02_Example`
folder). All logic files (`pet`, `habits`, `storage`, `ui`, `wifi_sync`)
are hardware-agnostic and port unchanged.

Verified 1.43C facts (from its schematic/docs — kept here so they aren't
lost):

- Same 466×466 CO5300 QSPI display as the 1.75, so `ui.cpp` and `web_sim`
  need zero changes.
- Touch controller is a **CST820** — not the CST9217 an earlier revision of
  this README claimed.
- I2C: SDA=GPIO47, SCL=GPIO48, with 4.7K pullups onboard.
- **No RTC** and no 32 kHz crystal, **no IMU**, **no AXP2101** — the same
  earlier revision wrongly claimed PCF85063/QMI8658/AXP2101 onboard. On the
  1.43C, the RTC / IMU / battery-gauge roadmap items all need external
  hardware.
- Battery sensing is a ~200K/200K divider into GPIO4 (BAT_ADC), not a PMIC.
- Dual mic array + ES7210 echo cancellation **is** present.
- No TF card slot.

## A note on the Waveshare code's license

The `waveshareteam/ESP32-S3-Touch-AMOLED-1.75` repo carries an **Apache-2.0
LICENSE file** (checked on GitHub, July 2026), so using and adapting its
drivers/examples is straightforward — just keep Apache's license/notice
requirements if you redistribute their code. For contrast (relevant if you
port back to the 1.43C): the `ESP32-S3-Touch-AMOLED-1.43C` repo had **no
LICENSE file** as of July 2026 (3 commits, v1.1.0 dated 2026-04-20, folders
01–05 incl. schematics and structural designs), and unlicensed public code
strictly defaults to all-rights-reserved — don't redistribute that board's
driver code without checking with Waveshare. Everything in *this* project
(pet/habits/storage/ui/wifi_sync) is original and yours to license however
you want.

## Editing default habits

Edit the `habits.init()` seed list in `habits.cpp`:

```cpp
addHabit("Drink water", 5);
addHabit("Move body", 10);
```

Name max length is 24 chars (`HABIT_NAME_LEN`), and up to 8 habits total
(`MAX_HABITS`) — bump these in `habits.h` if you want more, just mind flash
usage on saves.
