# CyberPet Dashboard

A Docker-hosted web control panel for the CyberPet habit-tracking device.
Add/remove habits and goals, tweak settings, and watch your pet's synced
state — all from a browser on your local network.

## Run it

```bash
docker compose up -d --build
```

Then open `http://localhost:8090` (or `http://<your-machine's-LAN-IP>:8090`
from another device, e.g. your phone).

Data persists in a Docker volume (`cyberpet-data`), so habits/goals/settings
survive container restarts. To reset everything, remove the volume:

```bash
docker compose down -v
```

## Without docker-compose

```bash
docker build -t cyberpet-dashboard .
docker run -d -p 8090:8080 -v cyberpet-data:/app/data --name cyberpet-dashboard cyberpet-dashboard
```

## Connecting the device

1. Find the LAN IP of the machine running this container (not `localhost` —
   the ESP32 needs a real IP on your network, e.g. `192.168.1.50`).
2. In `CyberPet.ino` (the firmware project), set:
   ```cpp
   const char* WIFI_SSID = "YourNetwork";
   const char* WIFI_PASSWORD = "YourPassword";
   const char* DASHBOARD_URL = "http://192.168.1.50:8090";
   ```
3. Install the **ArduinoJson** library (by Benoit Blanchon) via Arduino
   IDE's Library Manager — `wifi_sync.cpp` depends on it.
4. Reflash. On boot the device will connect to WiFi and start syncing every
   60 seconds (adjustable via `SYNC_INTERVAL_MS` in the sketch, or from the
   dashboard's Settings panel once synced state is wired up further).

If `WIFI_SSID` is left blank, the device just runs standalone with no
network calls — the dashboard becomes optional, not required.

## API reference

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/habits` | List active habits |
| POST | `/api/habits` | Add habit `{name, xpValue}` |
| PATCH | `/api/habits/:id` | Edit habit `{name?, xpValue?}` |
| DELETE | `/api/habits/:id` | Remove habit (soft delete) |
| GET | `/api/goals` | List active goals |
| POST | `/api/goals` | Add goal `{name, xpValue, period}` |
| DELETE | `/api/goals/:id` | Remove goal |
| GET | `/api/settings` | Read current settings |
| POST | `/api/settings` | Update settings (partial) |
| GET | `/api/pet` | Last-synced pet state |
| POST | `/api/sync` | Device sync endpoint: accepts `{deviceId, petState, completedHabits: [names]}`, returns current habits/goals/settings |

## Fixed in the v3 revision

- **Dashboard XP was silently discarded by the device** — XP awarded via quests
  or manual habit completions was written into `petState` on the dashboard, but
  the device overwrote `petState` on every sync so the XP vanished within one
  cycle. Fixed with a monotonic `dashXpTotal` counter: the dashboard accumulates
  all awarded XP; the device applies only the delta above what it has already
  received (`applyDashboardXpTotal` in `pet.cpp`) — idempotent, no ack needed.
  Quest completions also bump `configVersion` so the device's 5 s config poll
  delivers the XP promptly.
- **Long habit names produced null completions and zero streaks** — `recordCompletions`
  and `streakFor` used strict `===` matching against names that may have been
  truncated to 23 chars on-device. Replaced with `namesMatch()`, a
  truncation-aware helper consistent with the firmware's `strncmp` rule.
- **Sync response overflow killed sync silently** — the ArduinoJson response
  buffer was `StaticJsonDocument<2048>`; a response including habits + goals +
  quests + settings easily exceeds that, causing `deserializeJson` to fail and
  the device to stop syncing. Replaced with `DynamicJsonDocument(6144)`.
- **docker run port** — `README.md` listed `-p 8080:8080` (host 8080 is taken by
  SearXNG); corrected to `-p 8090:8080`.

## Fixed in the v2 revision

- Device sync now sends habit **names** as completions (`completedHabits`)
  instead of internal slot indices; the server accepts both for
  compatibility.
- `/api/settings` now whitelists known keys and drops null/empty/NaN
  values, so a half-filled dashboard form can't wipe out valid settings.
  The frontend also strips empty fields before sending.
- Firmware-side name comparison is now truncation-aware, fixing a bug
  where habits with names >23 chars were removed and re-added (streak
  wiped) on every single sync.

## Known gaps / next steps

- **Habit reconciliation is name-based**, not ID-based. Adding/removing
  habits from the dashboard works fine, but *renaming* one shows up
  on-device as remove-old + add-new (streak resets). Fix: add a `serverId`
  field to the on-device `Habit` struct and match on that.
- **Goals aren't synced to the device yet** — the API and dashboard UI for
  goals are complete, but `wifi_sync.cpp` only pulls/pushes habits and pet
  state right now.
- **Completion history isn't stored** — the server only keeps the *latest*
  completion list (`lastCompletedHabits`). If you want streak charts on
  the dashboard, append to a dated history array in `store.js` instead.
- **No auth** — designed for a trusted local network (your home WiFi).
  Don't expose port 8090 (host side) to the public internet without adding
  authentication first.
- **Settings aren't yet consumed by the firmware** — `moodGainPerHabit`,
  `moodDecayPerMiss`, `dailyResetHour` etc. exist in the dashboard/API but
  `pet.cpp` still has these hardcoded. Wire `wifi_sync.cpp`'s settings
  response into `Pet`/`HabitTracker` setters to make them actually
  adjustable from the dashboard.
- **Storage is a single JSON file** — fine for one device and one
  household. For history/analytics, swap `store.js` for a real database;
  it's isolated behind `get`/`update`, so it's a one-file change.
