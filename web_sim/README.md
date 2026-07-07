# CyberPet Web Simulator

Runs the **real firmware UI** — the same `ui.cpp`, `pet.cpp`, `habits.cpp`
that get flashed to the device, unchanged — in a browser via LVGL +
Emscripten/WebAssembly, or as a native desktop window. Use it to iterate on
screens/animations/logic without reflashing hardware, or to demo the pet
without the device in hand.

The page masks the canvas to a circle to mimic the 1.43" round AMOLED.

## What's real and what's simulated

| | |
|---|---|
| Pet logic, evolution, mood | **Real** — same `pet.cpp` |
| Habit list, streaks, daily reset | **Real** — same `habits.cpp` |
| All screens, touch behavior | **Real** — same `ui.cpp`, mouse = touch |
| LVGL version | v8.3.11 (pinned in CMake) |
| Flash persistence (NVS) | **Not simulated** — state resets on reload |
| WiFi/dashboard sync | **Not built** — firmware-only |
| RTC / real day rollover | **Simulated** — press `d` |
| Pomodoro countdown ring | **Real** — same `ui.cpp`; 30s/10s in sim mode |

Sim-only keyboard shortcuts:
- `d` — advance one day (dailyTick + habit reset; test streaks/mood decay)
- `x` — add 25 XP (test evolution stages quickly)
- `space` — add a workout rep (when workout screen is active)
- `p` — fire IMU guilt-trip (flashes Pomodoro arc red + "⚠ FOCUS!" for 1.5s; tests `pomodoroGuiltTrip()`)

The native build compiles with `POMODORO_SIM_MODE`, which shortens Pomodoro
intervals to 30 s focus / 10 s break so you can test a full cycle without
waiting 25 minutes. The wasm build uses the real 25 min / 5 min durations.

## Build → WebAssembly

No Emscripten install needed if you have Docker:

```bash
./build.sh docker
cd dist && python3 -m http.server 8000
# open http://localhost:8000/cyberpet_sim.html
```

With a local Emscripten install (`yay -S emscripten` on Arch): `./build.sh`

To serve it from the dashboard container instead, copy `dist/*` into
`../CyberPetDashboard/public/sim/` and rebuild the container — it'll be at
`http://<host>:8090/sim/cyberpet_sim.html`.

## Build → native desktop window (fastest iteration)

```bash
# needs: cmake, sdl2 (pacman -S cmake sdl2)
cmake -B build_native && cmake --build build_native -j
./build_native/cyberpet_sim
```

## How it works

- `main_web.cpp` — entry point: a ~60-line SDL display driver (flush LVGL's
  buffer into an SDL texture) + mouse-as-touchscreen input + the
  emscripten/native main loop. No `lv_drivers` dependency.
- `shim/Arduino.h` — minimal shim (`min`/`max`, stdint/string) so the
  hardware-agnostic firmware files compile off-device. Firmware-only files
  (`storage.cpp`, `wifi_sync.cpp`, `CyberPet.ino`) are excluded by design —
  don't try to shim `Preferences`/`WiFi`.
- `lv_conf.h` — LVGL config (32-bit color for direct SDL texture mapping).
- `CMakeLists.txt` — fetches LVGL v8.3.11 automatically; builds for wasm
  under `emcmake` and native otherwise.

## Verified

- All three firmware files compile unchanged against real LVGL v8.3.11
  headers (this confirmed the `lv_obj_set_user_data` v8.2+ note in `ui.h`).
- Full native build links and runs (headless smoke test passed).
- The **wasm build itself hasn't been run here** — the Emscripten toolchain
  wasn't available in the environment this was authored in. The CMake/SDL2
  emscripten flags follow the standard `-sUSE_SDL=2` pattern; if the docker
  build hits anything, it'll be in the link flags, not the app code.

## Caveats

- The sim uses whatever fonts/widgets `lv_conf.h` enables here, which may
  differ slightly from Waveshare's own `lv_conf.h` on-device (anti-aliasing,
  default font size). Pixel-perfect parity would need copying their config
  values into this one.
- If the firmware later moves to the LVGL version Waveshare ships (if it's
  not 8.3), bump the `GIT_TAG` in CMakeLists to match so sim == device.
