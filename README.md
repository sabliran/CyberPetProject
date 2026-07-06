# CyberPet

A habit-tracking virtual pet for the **Waveshare ESP32-S3-Touch-AMOLED-1.43C**.
Complete your daily habits, earn XP, and watch your blob evolve — with an
optional web dashboard for managing everything from your browser.

Fully local and self-hostable. No license key, no subscription, no cloud.

## What's in here

This project has two independent parts, each in its own folder with its own
detailed README:

| Folder | What it is | Runs on | README |
|---|---|---|---|
| **`CyberPet/`** | The device firmware — pet logic, habit tracking, LVGL UI, on-device storage, optional WiFi sync | The ESP32 device | [CyberPet/README.md](CyberPet/README.md) |
| **`CyberPetDashboard/`** | The web control panel — add/remove habits & goals, edit settings, view pet status | Docker on your PC/server | [CyberPetDashboard/README.md](CyberPetDashboard/README.md) |
| **`web_sim/`** | Browser/desktop simulator — runs the real firmware UI via LVGL + Emscripten, no hardware needed | Browser (wasm) or native window | [web_sim/README.md](web_sim/README.md) |

They work independently: the firmware runs fully standalone if you never
touch the dashboard, and the dashboard is an optional companion for managing
habits from a browser instead of editing them in code.

## Quick start

**Just the device (standalone):**

1. Get Waveshare's example project for the board (see the firmware README).
2. Drop the `CyberPet/` files into it, wire up the two integration points.
3. Leave `WIFI_SSID` blank in `CyberPet.ino`, flash over USB-C. Done.

**With the dashboard too (hybrid):**

1. Run the dashboard: `cd CyberPetDashboard && docker compose up -d --build`
   → opens at `http://localhost:8090`.
2. In `CyberPet.ino`, set your WiFi credentials and the dashboard's LAN IP.
3. Flash. The device syncs habits/goals/settings down and pet state up
   every 60 seconds.

Full instructions, setup steps, and the API reference are in each folder's
README.

## How it fits together

```
   ┌─────────────────────┐         WiFi / HTTP          ┌──────────────────────┐
   │   CyberPet device    │  ──────  POST /api/sync  ──▶ │  CyberPetDashboard    │
   │  (ESP32, this repo)  │                              │  (Docker, your PC)    │
   │                      │  ◀── habits/goals/settings ─ │                      │
   │  • pet + habits      │                              │  • web control panel  │
   │  • LVGL touch UI     │                              │  • REST API           │
   │  • local NVS storage │                              │  • JSON file storage  │
   └─────────────────────┘                              └──────────────────────┘
         source of truth for                                source of truth for
         streaks & completions                              which habits/goals exist
```

## Project status

Working starter project — device firmware and dashboard both functional and
tested. See each README's "Known gaps" and "Future additions" sections for
what's next. The single highest-impact improvement is wiring the onboard
PCF85063 RTC for accurate midnight habit resets (currently uptime-based).

## Hardware

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.43C (466×466 round AMOLED,
  capacitive touch, ESP32-S3, 16MB flash, 8MB PSRAM)
- **Onboard chips used or usable:** CO5300 display (QSPI), CST9217 touch
  (I2C), QMI8658 IMU, PCF85063 RTC, AXP2101 power management
- Storage footprint is tiny (saved state well under 1KB) — see the firmware
  README's storage notes. An SD card is only worth adding if you later do
  sprite art or audio.

## License

All original project code (`pet`, `habits`, `storage`, `ui`, `wifi_sync`,
the dashboard, this repo) is yours to license as you wish. Note that
Waveshare's own driver/example code — which the firmware builds on top of —
has its own licensing; see the note at the bottom of the firmware README
before redistributing any of their code.
