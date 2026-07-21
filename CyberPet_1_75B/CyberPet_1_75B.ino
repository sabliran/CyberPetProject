/*
  CyberPet - 1.75B edition
  Board: Waveshare ESP32-S3-Touch-AMOLED-1.75B
  (466x466 round AMOLED, CO5300 over QSPI via Arduino_GFX, CST92xx touch,
   AXP2101 PMU battery/fuel gauge, PCF85063 RTC — pins in pin_config.h)

  Integration comes from Waveshare's plain-1.75 Arduino examples
  (06_LVGL_Widgets, 05_AXP2101, 03_PCF85063). No separate 1.75B demo exists
  publicly; the B revision is assumed demo-compatible with the plain 1.75.
  ⚠ UNVERIFIED on the B hardware until first boot — if display or touch stay
  dead, get the demo from the board's own wiki page and diff the init.

  Unlike the 1.43C BSP there is NO FreeRTOS display task here: LVGL is pumped
  from loop() (lv_timer_handler below), so no display locking is needed —
  but long blocking calls (WiFi HTTP) freeze animations while they run.
*/

#include <lvgl.h>
#include <Wire.h>
#include "pin_config.h"          // also defines XPOWERS_CHIP_AXP2101 (before XPowersLib)
#include <Arduino_GFX_Library.h>
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"
#include "XPowersLib.h"
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>   // RTC-domain pull config for the tap-to-wake pin
#include <ArduinoOTA.h>
#include "ESP_I2S.h"
#include "esp_check.h"
#include "es8311.h"   // codec driver bundled from Waveshare's example 08
#include <math.h>

#include "pet.h"
#include "habits.h"
#include "storage.h"
#include "ui.h"
#include "wifi_sync.h"
#include "timekeeping.h"

// WiFi credentials live in the gitignored secrets.h (copy secrets.h.example).
// Leave WIFI_SSID_SECRET empty there to run fully standalone.
#include "secrets.h"
const char* WIFI_SSID     = WIFI_SSID_SECRET;
const char* WIFI_PASSWORD = WIFI_PASSWORD_SECRET;
// *.local hostnames are resolved via mDNS after WiFi connects, so this stays
// valid when the host's DHCP address changes.
const char* DASHBOARD_URL = "http://omarchy.local:8090";

// POSIX TZ string — Europe/Athens (EET, with EU DST rules). Keeps the
// pet-screen clock, daily resets and the RTC stamp all in local time.
const char* TIMEZONE = "EET-2EEST,M3.5.0/3,M10.5.0/4";

// ── Board drivers (from Waveshare's 1.75 example 06) ────────────────────────
static Arduino_DataBus* bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
// Panel stays in native orientation — the CO5300 driver's rotation=2
// mirrors instead of rotating. The 180° flip (user docks USB-right) is done
// by LVGL software rotation instead (disp_drv.rotated below): pixel-exact
// regardless of controller MADCTL quirks.
static Arduino_CO5300* gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
static TouchDrvCST92xx touch;
static XPowersAXP2101  power;
static bool pmuOk = false;

static volatile bool touchPressed = false;  // set by TP_INT falling-edge ISR

// ── Walk app: QMI8658 hardware pedometer ───────────────────────────────────
// The chip counts steps in silicon; loop() polls the counter every 2 s and
// accumulates deltas into stepState (the raw counter resets at power-off).
static SensorQMI8658 qmi;
static bool      imuOk      = false;
static uint8_t   imuAddr    = 0;   // I2C address begin() succeeded at
static StepState stepState;        // today's steps, NVS-backed (storage.h)

// Device settings (swipe-up screen), NVS-backed. Loaded in setup(); the UI
// fires settingsChangedCB (below, near pocket mode) on every user change.
static DeviceSettings devCfg = { 100, 0, 200, 0, 2, 1, 1 };
static uint32_t  pedLastRaw = 0;   // last raw step-counter reading

// Software step detector. The QMI8658's silicon pedometer never produced a
// count on this board (config commands ack fine, engine bit sets, counter
// stays 0 — see git history), so steps are detected in firmware instead:
// acceleration magnitude, slow-EMA gravity baseline, hysteresis peak
// detection with a walking-cadence gate. Sampled from loop() every ~15 ms.
static uint32_t swSteps      = 0;     // monotonic counter, this power cycle
static float    swBaseline   = 1.0f;  // slow EMA of |a| (tracks gravity)
static float    swFilt       = 0.0f;  // smoothed deviation from baseline
static bool     swAbove      = false; // hysteresis state
static uint32_t swLastStepMs = 0;
static uint32_t swLastSample = 0;

extern PetUI ui;  // defined below with the other globals; the swing detector needs it

// Motion capture (pull-up detector tuning): while a pull-up session runs,
// every |a| sample is recorded as milli-g into PSRAM; when the session ends
// the loop ships it to the dashboard (POST /api/motionlog) for offline
// analysis. Costs nothing when no session is running.
static const int  CAP_MAX = 16384;      // ~4 min at the sampler's ~66 Hz
static uint16_t*  capBuf  = nullptr;    // ps_malloc'd on first session
static int        capLen  = 0;

static void swStepSample() {
  uint32_t nowMs = millis();
  if (nowMs - swLastSample < 15) return;   // ~66 Hz effective sampling
  swLastSample = nowMs;

  float ax = 0, ay = 0, az = 0;
  if (!qmi.getAccelerometer(ax, ay, az)) return;
  float mag = sqrtf(ax * ax + ay * ay + az * az);   // ~1.0 g at rest

  swBaseline = 0.99f * swBaseline + 0.01f * mag;    // ~1 s time constant
  swFilt     = 0.70f * swFilt + 0.30f * (mag - swBaseline);

  // Rising edge through +60 mg = step candidate; must fall below +20 mg
  // before the next one counts (hysteresis). Cadence gates: candidates
  // closer than 250 ms apart are jitter, and counting only begins after
  // SW_ENTRY_STEPS consecutive candidates at walking cadence — one-off
  // bumps (picking the device up, desk knocks) never reach the total.
  // Once walking is established the warm-up burst is credited
  // retroactively and every step counts live; a gap longer than
  // SW_CADENCE_MS ends the walk and re-arms the entry filter.
  static const uint32_t SW_ENTRY_STEPS = 10;
  static const uint32_t SW_CADENCE_MS  = 2500;
  static bool     swWalking = false;
  static uint32_t swBurst   = 0;

  // Step counter toggled off (settings screen): the sampler may still be
  // running for a focus/back session — don't let that motion leak into
  // swSteps, or it gets credited as steps when the counter is re-enabled.
  if (!devCfg.stepsOn) {
    swWalking = false; swBurst = 0;
  } else if (!swAbove && swFilt > 0.06f) {
    swAbove = true;
    if (nowMs - swLastStepMs > 250) {
      if (nowMs - swLastStepMs > SW_CADENCE_MS) { swWalking = false; swBurst = 0; }
      if (swWalking) {
        swSteps++;
      } else if (++swBurst >= SW_ENTRY_STEPS) {
        swWalking = true;
        swSteps += swBurst;   // credit the warm-up steps retroactively
        swBurst = 0;
      }
      swLastStepMs = nowMs;
    }
  } else if (swAbove && swFilt < 0.02f) {
    swAbove = false;
  }

  // Wide-swing rep detector (back-workout app): a rep = raw |a| spiking
  // past the trigger and settling back under the re-arm level. Tuned on
  // user data: 1.7 g caught only 4 of 12 real rows — most swing peaks sit
  // lower, so trigger at 1.35 g (still ~9× walking's ~±150 mg deviations).
  // Only armed while the back screen is in a running session.
  // Focus guilt-trip: picking the device up during a running focus block
  // upsets the blob (red arc + "FOCUS!"). Resting |a| is ~1 g; a pickup
  // deviates well past ±0.35 g. 30 s cooldown so one grab scolds once.
  static uint32_t lastGuiltMs = 0;
  if (ui.isFocusRunning() && fabsf(mag - 1.0f) > 0.35f &&
      nowMs - lastGuiltMs > 30000UL) {
    lastGuiltMs = nowMs;
    ui.pomodoroGuiltTrip();
  }

  // Back-workout rep detector: calm-gated spike counting. A rep counts the
  // moment |a| crosses the trigger, but only if the device first spent
  // >= 350 ms continuously below the re-arm level ("calm"). Inside a rep
  // the pull and the return are one continuous motion with no calm stretch
  // between them, so they can't double-count — which pure time refractories
  // couldn't guarantee (a slow rep outlasts any refractory: 20 reps counted
  // 25 at 900 ms). Between reps the hand rests on the floor: always calm.
  static const float    SWING_TRIGGER_G = 1.35f;
  static const float    SWING_REARM_G   = 1.10f;
  static const uint32_t CALM_MS         = 350;
  static uint32_t quietSince = 0;      // start of current below-rearm stretch (0 = moving)
  static bool     armedByCalm = false;
  // Accuracy note (July 2026, tuned on captured signal data): ~±15% —
  // 20 real reps count 22-23. The user's real sets mix ~0.9 s and ~2 s rep
  // spacing at full swing strength, so no timing rule can separate the few
  // phantom triggers from genuine fast reps. Accepted as good enough.
  if (ui.isBackRunning()) {
    if (mag < SWING_REARM_G) {
      if (quietSince == 0) quietSince = nowMs;
      if (nowMs - quietSince >= CALM_MS) armedByCalm = true;
    } else {
      quietSince = 0;
      if (armedByCalm && mag > SWING_TRIGGER_G) {
        armedByCalm = false;   // consumed; re-arms only after the next calm stretch
        ui.addBackRep();
      }
    }
  } else {
    quietSince = 0;
    armedByCalm = false;
  }

  // Pull-up rep detector (device in a pocket): a rep is the concentric pull —
  // an upward burst (|a| > trigger) that must be followed by the top-of-bar
  // deceleration dip (|a| < dip) within the window. Burst + dip are one
  // continuous motion, so this fires exactly once per pull. The back
  // workout's plain calm gate can't work here: controlled lowering sits near
  // 1 g (reads as calm), so the bottom "catch" bump would double-count — but
  // the catch has no dip after it (the dead hang is a steady 1 g), so its
  // pending burst just expires.
  // v3, tuned on a captured real set (July 2026, 5 strict reps in a pocket;
  // /api/motionlog): pull peaks were 1.20-1.31 g (v1's 1.25 trigger missed
  // 3 of 5), post-pull dips 0.69-0.82 g, reps ~3 s apart — while pocket
  // handling spiked 1.42-1.45 g, i.e. HARDER than real pulls. Hence the
  // peak ceiling: a burst that tops 1.38 g is handling, not a pull-up. The
  // 2 s min gap absorbs sub-ceiling handling chatter. Replayed against the
  // capture this scores 5/5 with zero phantoms (start handling stopped by
  // the stillness gate, bar dismount by dip-before-spike ordering).
  static const float    PULL_TRIGGER_G      = 1.16f;
  static const float    PULL_REARM_G        = 1.08f;
  static const float    PULL_DIP_G          = 0.88f;
  static const float    PULL_CEILING_G      = 1.38f;
  static const uint32_t PULL_DIP_WINDOW_MS  = 1200;
  static const uint32_t PULL_STILL_MS       = 1200;
  static const uint32_t PULL_MIN_GAP_MS     = 2000;
  static bool     pullSessionArmed = false;  // stillness gate, once per session
  static uint32_t pullStillSince   = 0;
  static bool     pullArmed   = true;
  static uint32_t pullPending = 0;   // burst time; 0 = no burst waiting for its dip
  static float    pullBurstMax = 0;  // peak |a| inside the pending burst
  static uint32_t pullLastRepMs = 0;
  // Record whole workout sessions (pull-up AND back), handling included —
  // that's tuning gold, and the dashboard's Motion Lab plots it.
  if ((ui.isPullupRunning() || ui.isBackRunning()) && capBuf && capLen < CAP_MAX) {
    float mg = mag * 1000.0f;
    capBuf[capLen++] = (uint16_t)(mg < 0 ? 0 : (mg > 65535.0f ? 65535 : mg));
  }

  if (ui.isPullupRunning()) {
    if (!pullSessionArmed) {
      // Pocketing the device / walking to the bar is continuous jiggle;
      // counting arms only after 1.2 s of holding still (standing or dead
      // hang), so handling can't score reps.
      if (fabsf(mag - 1.0f) < 0.12f) {
        if (pullStillSince == 0) pullStillSince = nowMs;
        if (nowMs - pullStillSince >= PULL_STILL_MS) pullSessionArmed = true;
      } else {
        pullStillSince = 0;
      }
    } else {
      if (mag < PULL_REARM_G) pullArmed = true;
      if (pullArmed && mag > PULL_TRIGGER_G && pullPending == 0) {
        pullArmed = false;           // one pending per burst, however long it lasts
        pullPending = nowMs;
        pullBurstMax = mag;
      }
      if (pullPending != 0) {
        if (mag > pullBurstMax) pullBurstMax = mag;
        if (mag < PULL_DIP_G) {
          if (pullBurstMax <= PULL_CEILING_G &&
              nowMs - pullLastRepMs >= PULL_MIN_GAP_MS) {
            pullLastRepMs = nowMs;
            ui.addPullupRep();
          }
          pullPending = 0;
        } else if (nowMs - pullPending > PULL_DIP_WINDOW_MS) {
          pullPending = 0;           // burst without a dip: catch bump or noise
        }
      }
    }
  } else {
    pullSessionArmed = false;
    pullStillSince = 0;
    pullArmed = true;
    pullPending = 0;
    pullBurstMax = 0;
    pullLastRepMs = 0;
  }

}

// Direct register read, bypassing SensorLib — ground truth for debugging
// (the lib's CTRL9 command handshake can time out silently at the default
// log level, leaving the pedometer half-configured with no trace).
static uint8_t imuReadReg(uint8_t reg) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xEE;
  if (Wire.requestFrom((int)imuAddr, 1) != 1) return 0xEE;
  return Wire.read();
}

static bool imuWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// CTRL9 command with STATUSINT-bit7 handshake, logging on timeout.
// Precondition: CTRL8 bit7 (CTRL9_HandShake_Type) must be 1 — the chip's
// power-on default routes "command done" to the INT1 *pin* instead, which is
// not wired for polling here and is the suspected reason SensorLib's own
// writeCommand silently failed to configure the pedometer.
static bool imuCtrl9Cmd(uint8_t cmd) {
  imuWriteReg(0x0A, cmd);                       // CTRL9 = command
  uint32_t t0 = millis();
  while (!(imuReadReg(0x2D) & 0x80)) {          // STATUSINT.CmdDone
    if (millis() - t0 > 300) { Serial.printf("ped: CTRL9 0x%02X done-timeout\n", cmd); return false; }
    delay(1);
  }
  imuWriteReg(0x0A, 0x00);                      // CTRL_CMD_ACK
  t0 = millis();
  while (imuReadReg(0x2D) & 0x80) {             // wait for CmdDone to clear
    if (millis() - t0 > 300) { Serial.printf("ped: CTRL9 0x%02X ack-timeout\n", cmd); return false; }
    delay(1);
  }
  return true;
}

// Pedometer parameter upload + engine enable, done with direct register
// access (mirrors SensorLib's configPedometer, but with a working handshake
// and verified read-backs). Values are in the chip's native units:
// peaks in 1/1024 g, times in accel-ODR samples.
static bool imuPedometerInit(uint16_t sampleCnt, uint16_t peak2peak, uint16_t peak,
                             uint16_t timeUp, uint8_t timeLow, uint8_t cntEntry,
                             uint8_t precision, uint8_t sigCount) {
  // Handshake via STATUSINT (CTRL8 bit7) before any CTRL9 command.
  uint8_t ctrl8 = imuReadReg(0x09);
  imuWriteReg(0x09, ctrl8 | 0x80);

  // Sensors off during parameter upload (same as SensorLib).
  uint8_t ctrl7 = imuReadReg(0x08);
  imuWriteReg(0x08, ctrl7 & ~0x03);

  bool ok = true;
  imuWriteReg(0x0B, sampleCnt & 0xFF);  // CAL1_L
  imuWriteReg(0x0C, sampleCnt >> 8);    // CAL1_H
  imuWriteReg(0x0D, peak2peak & 0xFF);  // CAL2_L
  imuWriteReg(0x0E, peak2peak >> 8);    // CAL2_H
  imuWriteReg(0x0F, peak & 0xFF);       // CAL3_L
  imuWriteReg(0x10, peak >> 8);         // CAL3_H
  imuWriteReg(0x11, 0x02);              // CAL4_L: pedometer config, page 1
  imuWriteReg(0x12, 0x01);              // CAL4_H
  ok &= imuCtrl9Cmd(0x0D);              // CTRL_CMD_CONFIGURE_PEDOMETER

  imuWriteReg(0x0B, timeUp & 0xFF);
  imuWriteReg(0x0C, timeUp >> 8);
  imuWriteReg(0x0D, timeLow);
  imuWriteReg(0x0E, cntEntry);
  imuWriteReg(0x0F, precision);
  imuWriteReg(0x10, sigCount);
  imuWriteReg(0x11, 0x02);              // CAL4_L: pedometer config, page 2
  imuWriteReg(0x12, 0x02);              // CAL4_H
  ok &= imuCtrl9Cmd(0x0D);

  imuWriteReg(0x08, ctrl7);             // restore accel/gyro enables

  // Pedometer engine on (CTRL8 bit4), keep bit7 handshake routing.
  imuWriteReg(0x09, (ctrl8 | 0x80) | 0x10);
  uint8_t verify = imuReadReg(0x09);
  Serial.printf("ped: init %s, ctrl8 0x%02X (want bit4)\n", ok ? "ok" : "CTRL9 FAILED", verify);
  return ok && (verify & 0x10);
}

// ── Audio: ES8311 codec + NS4150 amp, short chirps on habit events ─────────
static I2SClass i2s;
static bool audioOk = false;
static const int AUDIO_RATE = 16000;

static esp_err_t codecInit() {
  es8311_handle_t es = es8311_create(0, ES8311_ADDRRES_0);
  if (!es) return ESP_FAIL;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = AUDIO_RATE * 256,
    .sample_frequency = AUDIO_RATE,
  };
  ESP_RETURN_ON_ERROR(es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), "ES8311", "init");
  ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency), "ES8311", "freq");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es, 75, NULL), "ES8311", "volume");
  return ESP_OK;
}

// One decaying sine burst, generated on the fly and written synchronously.
// Longest chirp is ~150 ms — an acceptable pause in the loop()-driven LVGL.
static void playTone(float freqHz, int ms, float vol) {
  static int16_t buf[2 * 2400];  // stereo frames, 150 ms @ 16 kHz max
  int frames = AUDIO_RATE * ms / 1000;
  if (frames > 2400) frames = 2400;
  for (int i = 0; i < frames; i++) {
    float env = 1.0f - (float)i / frames;  // linear decay, no click at the end
    int16_t s = (int16_t)(sinf(2.0f * (float)M_PI * freqHz * i / AUDIO_RATE) * 32767.0f * vol * env);
    buf[2 * i]     = s;
    buf[2 * i + 1] = s;
  }
  i2s.write((uint8_t*)buf, (size_t)frames * 2 * sizeof(int16_t));
}

// Registered with the UI layer (see PetSoundEvent in ui.h). Master volume
// and sound theme come from the settings screen. Themes: 0 classic
// (original chirps), 1 arcade (fast bright arpeggios), 2 soft (lower,
// gentler, quieter).
static void petSoundCB(int event) {
  if (!audioOk) return;
  float v = devCfg.volumePct / 100.0f;
  if (v <= 0.0f) return;
  int t = devCfg.soundTheme;
  switch (event) {
    case SOUND_HABIT_DONE:
      if      (t == 1) { playTone(1318.5f, 50, 0.5f * v); playTone(1568.0f, 50, 0.5f * v); playTone(2093.0f, 90, 0.5f * v); }
      else if (t == 2) { playTone(523.3f, 90, 0.35f * v); playTone(659.3f, 140, 0.35f * v); }
      else             { playTone(880.0f, 70, 0.5f * v);  playTone(1318.5f, 120, 0.5f * v); }
      break;
    case SOUND_HABIT_UNDONE:
      if      (t == 1) { playTone(392.0f, 60, 0.35f * v); playTone(261.6f, 80, 0.35f * v); }
      else if (t == 2) { playTone(330.0f, 110, 0.25f * v); }
      else             { playTone(523.3f, 90, 0.35f * v); }
      break;
    case SOUND_TROPHY:
      if      (t == 1) { playTone(523.3f, 60, 0.5f * v); playTone(659.3f, 60, 0.5f * v); playTone(784.0f, 60, 0.5f * v); playTone(1046.5f, 140, 0.5f * v); }
      else if (t == 2) { playTone(523.3f, 100, 0.35f * v); playTone(659.3f, 100, 0.35f * v); playTone(784.0f, 180, 0.35f * v); }
      else             { playTone(659.3f, 80, 0.5f * v); playTone(880.0f, 80, 0.5f * v); playTone(1318.5f, 150, 0.5f * v); }
      break;
    case SOUND_REP_BLIP:          // stays a whisper: 10% of master
      if      (t == 1) playTone(1568.0f, 45, 0.1f * v);
      else if (t == 2) playTone(784.0f, 70, 0.1f * v);
      else             playTone(1046.5f, 60, 0.1f * v);
      break;
    case SOUND_MOVE_ALERT:        // stand-up nag: insistent, same in all themes
      playTone(880.0f, 160, 0.7f * v);
      playTone(1174.7f, 160, 0.7f * v);
      playTone(880.0f, 240, 0.7f * v);
      break;
  }
}

Pet pet;
HabitTracker habits;
Storage storage;
PetUI ui;
WifiSync wifiSync;
RtcTimeKeeper timeKeeper;   // PCF85063 via Wire (started in setup)

static int  lastResetYear = -1;
static int  lastResetDOY  = -1;
static bool ntpEnabled    = false;
static int  lastTrophyCount = 0;   // for the new-trophy celebration

// Back-workout app: lifetime completed sessions, NVS-backed, reported in
// sync requests so the server can award back-workout trophies.
static uint32_t backSessions = 0;

static void backDoneCB() {
  backSessions++;
  storage.saveBackSessions(backSessions);
  wifiSync.setBackSessions(backSessions);
}

// Push-up app: same pattern.
static uint32_t pushSessions = 0;

static void pushDoneCB() {
  pushSessions++;
  storage.savePushSessions(pushSessions);
  wifiSync.setPushSessions(pushSessions);
}

// Pull-up app: same pattern.
static uint32_t pullupSessions = 0;

static void pullupDoneCB() {
  pullupSessions++;
  storage.savePullupSessions(pullupSessions);
  wifiSync.setPullupSessions(pullupSessions);
}

// OTA firmware updates over WiFi (requires the app3M_fat9M_16MB dual-slot
// partition scheme). Started once WiFi is up — boot-time or the loop's
// self-healing reconnect. Flash with:
//   arduino-cli upload -p cyberpet-device.local --fqbn ... CyberPet_1_75B
// (espota transport; password from secrets.h)
static bool otaStarted = false;

static void startOtaIfNeeded() {
  if (otaStarted) return;
  ArduinoOTA.setHostname("cyberpet-device");
  ArduinoOTA.setPassword(OTA_PASSWORD_SECRET);
  ArduinoOTA.onStart([]() {
    // Persist the lazily-saved state before the app partition is rewritten
    // (sleep/back/push/focus counters already save at the moment they
    // change, so only the change-guard-batched ones need a flush).
    storage.savePet(pet.getState());
    storage.saveHabits(habits);
    storage.saveStepState(stepState);
    Serial.println("OTA: update starting");
  });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.println("OTA: ready (cyberpet-device.local)");
}

// Focus app: completed 25-min blocks, same pattern.
static uint32_t focusSessions = 0;

static void focusDoneCB() {
  focusSessions++;
  storage.saveFocusSessions(focusSessions);
  wifiSync.setFocusSessions(focusSessions);
}

// Sleep app: once-per-day gate, NVS-backed. The UI applies the pet effects
// and fires this callback; we stamp today's date and persist both.
static SleepState sleepState;

static void sleepLogCB(int quality) {
  WallDate today = timeKeeper.now();
  sleepState.year      = today.valid ? today.year : 0;
  sleepState.dayOfYear = today.valid ? today.dayOfYear : 0;
  sleepState.quality   = quality;
  storage.saveSleepState(sleepState);
  storage.savePet(pet.getState());
  wifiSync.setSleepInfo(sleepState.quality, sleepState.year, sleepState.dayOfYear);
}
static uint32_t lastDailyCheck = 0;
static const uint32_t DAY_MS   = 24UL * 60UL * 60UL * 1000UL;

uint32_t lastSyncAttempt  = 0;
uint32_t lastSyncOkMs     = 0;  // last successful sync (settings-screen WiFi status)
const uint32_t SYNC_INTERVAL_MS     = 60UL * 1000UL;
const uint32_t SYNC_INTERVAL_USB_MS = 10UL * 1000UL;  // faster cadence when docked over USB
static int lastXpResetToken = 0;  // last dashboard XP-reset applied (NVS-backed)
uint32_t lastConfigCheck  = 0;
const uint32_t CONFIG_CHECK_MS   = 5UL * 1000UL;

// ── LVGL glue (from example 06, buffers shrunk — see heap note below) ───────

static void dispFlushCB(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
#if LV_COLOR_16_SWAP != 0
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)&color_p->full, w, h);
#endif
  lv_disp_flush_ready(disp);
}

// CO5300 wants even start / odd end coordinates (Waveshare's rounder).
static void dispRounderCB(struct _lv_disp_drv_t* disp_drv, lv_area_t* area) {
  if (area->x1 % 2 != 0) area->x1--;
  if (area->y1 % 2 != 0) area->y1--;
  if (area->x2 % 2 == 0) area->x2++;
  if (area->y2 % 2 == 0) area->y2++;
}

// Pocket mode (walk app): panel dark + touch ignored so steps count safely
// in a pocket. Entered via the walk screen's button (pocketModeCB below),
// exited by a BOOT short press, which lands back on the walk screen.
static bool pocketMode = false;

static void pocketModeCB() {
  pocketMode = true;
  touchPressed = false;   // drop any in-flight touch report
  gfx->setBrightness(0);  // AMOLED: black pixels + no backlight ≈ display off
  Serial.println("pocket mode ON (BOOT to wake)");
}

// Settings-screen restart (after the yes/no confirm): flush the lazily-saved
// state, then reboot. Same save set as the OTA prelude.
static void restartCB() {
  Serial.println("settings: user-requested restart");
  Serial.flush();
  storage.savePet(pet.getState());
  storage.saveHabits(habits);
  storage.saveStepState(stepState);
  ESP.restart();  // never returns
}

// Sit app display power: screen dark while the sit timer runs (battery),
// woken by BOOT or by the stand-up alert itself. Touch is suppressed while
// dark (see touchReadCB) so desk bumps can't tap buttons blind.
static bool sitDisplayOff = false;

static void sitScreenPowerCB(bool on) {
  sitDisplayOff = !on;
  gfx->setBrightness(on ? devCfg.brightness : 0);
  if (on) lv_disp_trig_activity(NULL);
  Serial.println(on ? "sit: display on" : "sit: display off (BOOT wakes)");
}

// Settings screen: persist + apply the hardware-facing knobs. Brightness
// lands live (slider preview); volume/theme/sleep are read at use time.
static void settingsChangedCB(const DeviceSettings& s) {
  // Turning the step counter off pauses the lazy once-a-minute persist below,
  // so flush any steps counted in the last minute before they stop mattering.
  if (devCfg.stepsOn && !s.stepsOn) storage.saveStepState(stepState);
  devCfg = s;
  storage.saveDeviceSettings(s);
  if (!pocketMode && !sitDisplayOff) gfx->setBrightness(devCfg.brightness);
}

static void touchReadCB(lv_indev_drv_t* indev, lv_indev_data_t* data) {
  // The CST92xx pulses TP_INT on every scan while touched, including a final
  // report with zero points on release — same read pattern as Waveshare's demo.
  static int16_t tx[5], ty[5];
  if (pocketMode || sitDisplayOff) {
    // Fabric brushing the glass (or a desk bump on a dark sit-mode screen)
    // must not tap anything. The controller still scans; we just never
    // report presses to LVGL.
    touchPressed = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  // Poll unconditionally on every read tick (30 ms). This used to be gated
  // on the TP_INT interrupt flag, but the INT line can come out of a
  // deep-sleep wake dead (July 2026, twice: device syncing happily, touch
  // gone until a hard reset) — and a dead INT gate means touch is lost for
  // good. The poll is a ~0.3 ms I2C read; against a lit AMOLED it's noise.
  touchPressed = false;
  uint8_t touched = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
  if (touched) {
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = tx[0];
    data->point.y = ty[0];
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ── Auto-sleep ────────────────────────────────────────────────────────────────
// Forgotten-on is the #1 battery killer: an idle pet screen still burns
// ~100 mA (AMOLED + WiFi + CPU). After AUTOSLEEP_MS without touch input the
// device saves everything and deep-sleeps (µA-level draw; wake is a fresh
// boot, all state lives in NVS). Wake sources: screen tap (ext0, TP_INT) and
// pick-up motion (optional — devCfg.liftWake, settings screen) — the
// QMI8658's INT2 is the only IMU interrupt routed to the
// ESP32 (GPIO21 per the 1.75 schematic; INT1 and the RTC INT only reach the
// TCA9554 expander, which can't wake the chip). Wake-on-motion runs the accel
// alone at a low-power ODR (tens of µA) — cheaper than keeping the touch
// controller scanning for tap-to-wake, which is why pickup is the gesture.
// Timeout now lives in devCfg.sleepMin (settings screen); the warning dim
// fires at 3/4 of it.
static const uint8_t  WOM_THRESHOLD_MG = 120;  // pick-up strength, not desk bumps

static bool wokeFromSleep = false;  // set at boot, picks the splash wording

static const char* resetReasonName(esp_reset_reason_t rr) {
  switch (rr) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    default:                return "other";
  }
}

// Last abnormal reset, loaded from NVS at boot (count 0 = never crashed).
// Printed at boot AND repeated every few minutes into the bridge log: a
// wake-crash happens on battery with no serial listener, so the record has
// to survive until the next dock to be seen.
static BootAnomaly lastAnomaly = {};

static void printCrashRecord() {
  if (lastAnomaly.count == 0) return;
  Serial.printf("crash record: %s at stage %u during a %s boot (x%u lifetime)\n",
                resetReasonName((esp_reset_reason_t)lastAnomaly.reason),
                lastAnomaly.stage, lastAnomaly.wasWake ? "wake" : "cold",
                (unsigned)lastAnomaly.count);
}

// Swap the IMU from pedometer duty to a wake-on-motion source. Direct
// registers, not qmi.configWakeOnMotion(): SensorLib resets the chip first,
// which restores the CTRL9-handshake-on-INT1-pin default, so its writeCommand
// times out silently (same trap as the pedometer init). Harmless across the
// wake reboot — setup()'s qmi.begin() resets the chip again.
static bool imuWakeOnMotionInit(uint8_t thresholdMg) {
  imuWriteReg(0x60, 0xB0);                     // soft reset
  uint32_t t0 = millis();
  while (imuReadReg(0x4D) != 0x80) {           // reset-done flag
    if (millis() - t0 > 500) { Serial.println("wom: reset timeout"); return false; }
    delay(10);
  }
  imuWriteReg(0x02, 0x40);                     // CTRL1: register auto-increment
  imuWriteReg(0x09, imuReadReg(0x09) | 0x80);  // CTRL8: CTRL9 handshake -> STATUSINT
  imuWriteReg(0x08, 0x00);                     // CTRL7: sensors off while configuring
  imuWriteReg(0x03, (2 << 4) | 13);            // CTRL2: ±8g, low-power 21 Hz
  imuWriteReg(0x0B, thresholdMg);              // CAL1_L: WoM threshold, 1 mg/LSB
  imuWriteReg(0x0C, (0x01 << 6) | 0x20);       // CAL1_H: INT2, idle low + 32-sample blanking (~1.5 s)
  if (!imuCtrl9Cmd(0x08)) return false;        // CTRL_CMD_WRITE_WOM_SETTING
  imuWriteReg(0x08, 0x01);                     // CTRL7: accel back on (low-power mode)
  imuWriteReg(0x02, imuReadReg(0x02) | 0x10);  // CTRL1: INT2 pin output enable
  return true;
}

static void enterAutoSleep() {
  Serial.println("autosleep: idle timeout - deep sleep (pick up or press BOOT to wake)");
  Serial.flush();
  storage.savePet(pet.getState());
  storage.saveHabits(habits);
  storage.saveStepState(stepState);
  gfx->setBrightness(0);
  gfx->displayOff();                            // CO5300 sleep-in
  if (devCfg.liftWake && imuOk && imuWakeOnMotionInit(WOM_THRESHOLD_MG)) {
    esp_sleep_enable_ext1_wakeup(1ULL << 21, ESP_EXT1_WAKEUP_ANY_HIGH);  // pickup
  } else if (imuOk) {
    // Lift-to-wake off (settings): power the accel down entirely for the
    // sleep — no WoM sampling, no motion wakeups in a bag or pocket.
    // Tap-to-wake below stays the wake path; next boot's qmi.begin()
    // resets the chip, so nothing needs undoing.
    imuWriteReg(0x08, 0x00);                    // CTRL7: all sensors off
  }
  // Tap-to-wake: the CST92xx stays powered through deep sleep (its rail is
  // never cut) and pulses TP_INT low on a touch, so this costs nothing extra.
  // Deliberately NOT the BOOT button: GPIO0 is a strapping pin, and a
  // human-length press still held when the wake reset samples it drops the
  // ROM into download mode — black screen, dead until a power cycle. That
  // trap is why BOOT-to-wake felt broken.
  rtc_gpio_pullup_en((gpio_num_t)TP_INT);
  rtc_gpio_pulldown_dis((gpio_num_t)TP_INT);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TP_INT, 0);
  esp_deep_sleep_start();                       // never returns; wake = fresh boot
}

static void lvglTickCB(void* arg) { lv_tick_inc(2); }

void setup() {
  // USB sync bridge sends ~1 KB responses in one burst; the HWCDC default
  // RX buffer (256 B) would overflow. Must be set before Serial.begin().
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);

  // Crash forensics: the USB bridge log keeps this line, so an unexpected
  // reboot names its cause even when the panic text is lost with the port.
  storage.begin();  // NVS up front — the breadcrumbs below need it
  {
    esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("boot: reset reason %d (%s)\n", (int)rr, resetReasonName(rr));
    if (rr == ESP_RST_DEEPSLEEP) {
      wokeFromSleep = true;
      esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
      Serial.printf("boot: autosleep wake by %s\n",
          wc == ESP_SLEEP_WAKEUP_EXT0 ? "screen tap" :
          wc == ESP_SLEEP_WAKEUP_EXT1 ? "pickup (IMU)" : "other");
    }
    // The previous boot's breadcrumb says how far it got (0 = reached
    // loop()); an abnormal reset pins it plus the reset reason in NVS so the
    // record survives battery-time crashes nobody was listening to.
    bool prevWake = false;
    uint8_t prevStage = storage.loadBootStage(&prevWake);
    // Always name how far the previous boot got — a "stuck but not crashed"
    // report (stage 0 = it was running loop()) is evidence too, and this
    // line costs nothing.
    Serial.printf("boot: previous boot reached stage %u (%s boot)\n",
                  prevStage, prevWake ? "wake" : "cold");
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT) {
      BootAnomaly a = storage.loadBootAnomaly();
      a.reason  = (uint8_t)rr;
      a.stage   = prevStage;
      a.wasWake = prevWake ? 1 : 0;
      a.count++;
      storage.saveBootAnomaly(a);
    }
    storage.saveBootStage(1, wokeFromSleep);  // stage 1: setup() entered
    lastAnomaly = storage.loadBootAnomaly();
    printCrashRecord();
  }

  // Deep-sleep wake leaves the ext0/ext1 wake pads under RTC-mux control —
  // the IDF docs require rtc_gpio_deinit() before the pad works as a digital
  // GPIO again. Without this the touch INT (GPIO11) can stay routed away
  // from the GPIO matrix after a wake: screen on, touch dead, looks crashed.
  if (wokeFromSleep) {
    rtc_gpio_deinit((gpio_num_t)TP_INT);
    rtc_gpio_pullup_dis((gpio_num_t)TP_INT);
    rtc_gpio_deinit(GPIO_NUM_21);  // QMI_INT2 (pickup wake)
  }

  Wire.begin(IIC_SDA, IIC_SCL);

  // Touch reset dance per Waveshare's example (pinMode added for correctness).
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(30);
  digitalWrite(TP_RESET, HIGH);
  delay(100);

  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)) {
    Serial.println("touch is not online... continuing without touch");
  } else {
    Serial.print("touch model: ");
    Serial.println(touch.getModelName());
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setMirrorXY(true, true);  // native mapping (example 06) — LVGL's
                                    // disp_drv.rotated transforms pointer
                                    // input itself; flipping here too would
                                    // double-flip (inverted swipes)
    // Explicit pad config: after a deep-sleep wake rtc_gpio_deinit hands the
    // pin back with the input path in an undefined state, and attachInterrupt
    // alone doesn't reconfigure it. (Touch reads no longer depend on this INT
    // — see touchReadCB — it only wakes the tap-to-wake path and costs nothing.)
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, []() { touchPressed = true; }, FALLING);
  }

  // AXP2101 PMU: battery percent + charge state. Non-fatal if absent.
  pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmuOk) {
    Serial.println("AXP2101 not found - battery readout disabled");
  } else {
    // PWR button = power switch: hold 4 s to cut all rails (hardware off).
    // Press PWR again (or plug USB) to power back on. This must be enabled
    // by firmware — the PMU default ignores long presses.
    power.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    power.setLongPressPowerOFF();
    power.enableLongPressShutdown();
    // Charge the RTC backup rail (AXP2101 button-battery charger) so the
    // PCF85063 can keep wall-clock time through power-off. Harmless if the
    // board routes nothing to VBACKUP.
    power.setButtonBatteryChargeVoltage(3300);
    power.enableButtonBatteryCharge();
  }

  // One-shot I2C bus scan for the boot log — ground truth on which
  // peripherals actually ACK. Expected: 0x18 ES8311, 0x34 AXP2101,
  // 0x51 PCF85063 RTC, 0x5A CST92xx touch, 0x6B QMI8658 IMU.
  {
    char line[128];
    int n = snprintf(line, sizeof(line), "i2c scan:");
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0 && n < (int)sizeof(line) - 6)
        n += snprintf(line + n, sizeof(line) - n, " 0x%02X", a);
    }
    Serial.println(line);
  }

  // BOOT button (GPIO0) doubles as a soft power-off — see loop().
  pinMode(0, INPUT_PULLUP);

  // QMI8658 IMU: hardware pedometer drives the walk app. Probe both slave
  // addresses; non-fatal if the board variant lacks the chip — the walk
  // screen then shows "no motion sensor".
  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuOk) {
    imuAddr = QMI8658_L_SLAVE_ADDRESS;
  } else if ((imuOk = qmi.begin(Wire, QMI8658_H_SLAVE_ADDRESS, IIC_SDA, IIC_SCL))) {
    imuAddr = QMI8658_H_SLAVE_ADDRESS;
  }
  if (imuOk) {
    // 125 Hz so the ~66 Hz software detector always sees a fresh sample.
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_2G, SensorQMI8658::ACC_ODR_125Hz);
    qmi.enableAccelerometer();
    // Based on SensorLib's pedometer-example tuning for the 62.5 Hz ODR, but
    // more sensitive: this device is carried in the hand (or sits in a
    // pocket), which produces weaker peaks than the wrist wear the example
    // values target. sig_count 1 = counter register updates every step
    // (we poll — the coarser cadence only helps interrupt users).
    // Configured with direct register access (imuPedometerInit above), NOT
    // qmi.configPedometer(): SensorLib's CTRL9 handshake times out silently
    // when the chip's handshake routing is at its INT1-pin default.
    imuPedometerInit(50,   // ped_sample_cnt
                     120,  // ped_fix_peak2peak (~117 mg; example 200)
                     60,   // ped_fix_peak (~59 mg; example 100)
                     200,  // ped_time_up (samples)
                     20,   // ped_time_low (samples)
                     4,    // ped_time_cnt_entry (steps; example 10)
                     0,    // ped_fix_precision
                     1);   // ped_sig_count
    // Register dump: WHO_AM_I(0x00)=0x05, REVISION(0x01), CTRL2(0x03)=accel
    // ODR/range, CTRL7(0x08) bit0=accel enable, CTRL8(0x09) bit4=pedometer
    // enable, STATUSINT(0x2D).
    Serial.printf("QMI8658 @0x%02X who 0x%02X rev 0x%02X ctrl2 0x%02X ctrl7 0x%02X ctrl8 0x%02X stint 0x%02X\n",
                  imuAddr, imuReadReg(0x00), imuReadReg(0x01), imuReadReg(0x03),
                  imuReadReg(0x08), imuReadReg(0x09), imuReadReg(0x2D));
    Serial.println("QMI8658 pedometer enabled");
  } else {
    Serial.println("QMI8658 not found - step counting disabled");
  }

  // Speaker path: amp enable, I2S out, then codec (needs MCLK running first —
  // same order as Waveshare's example 08). Non-fatal: no audio = silent pet.
  pinMode(PA, OUTPUT);
  digitalWrite(PA, HIGH);
  i2s.setPins(BCLKPIN, WSPIN, DIPIN, DOPIN, MCLKPIN);
  if (i2s.begin(I2S_MODE_STD, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH) &&
      codecInit() == ESP_OK) {
    audioOk = true;
  } else {
    Serial.println("audio init failed - habit sounds disabled");
  }

  storage.saveBootStage(2, wokeFromSleep);  // stage 2: peripherals up, display next

  gfx->begin();
  gfx->setBrightness(200);

  // Boot splash, raw GFX before LVGL exists: WiFi connect can hold setup()
  // for 15+ seconds and an all-black panel reads as "still off", which
  // invites more pressing. No text — this framebuffer is native orientation
  // while the UI runs 180°-rotated in LVGL, so glyphs would render upside
  // down; a blob face + loading dots read fine either way. Coordinates below
  // are user-space mirrored through (466-x, 466-y).
  {
    uint16_t blue = gfx->color565(66, 120, 235);   // sprite shade blue
    gfx->fillScreen(0x0000);
    gfx->fillCircle(233, 256, 46, blue);           // body, user (233,210)
    gfx->fillRect(244, 257, 8, 14, 0x0000);        // eyes, user y 195
    gfx->fillRect(214, 257, 8, 14, 0x0000);
    for (int i = 0; i < 3; i++)                    // "loading" dots, user y 300
      gfx->fillCircle(203 + 30 * i, 166, 6, blue);
  }

  lv_init();

  // 40 lines per buffer, NOT the example's full-height/4 monsters: two of
  // those cost ~217 KB of internal DMA RAM, which starved the 1.43C's heap
  // to ~4 KB once WiFi loaded (failed syncs + idle freezes). 40-line double
  // buffering = 74 KB total.
  static lv_disp_draw_buf_t draw_buf;
  lv_color_t* buf1 = (lv_color_t*)heap_caps_malloc(LCD_WIDTH * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
  lv_color_t* buf2 = (lv_color_t*)heap_caps_malloc(LCD_WIDTH * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(buf1 && buf2);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res    = LCD_WIDTH;
  disp_drv.ver_res    = LCD_HEIGHT;
  disp_drv.flush_cb   = dispFlushCB;
  disp_drv.rounder_cb = dispRounderCB;
  disp_drv.draw_buf   = &draw_buf;
  // 180° software rotation: the user docks with USB on the right, which is
  // upside-down for the panel. LVGL rotates each flushed area itself; the
  // even/odd rounder stays valid because the panel is 466 px (even) wide.
  disp_drv.sw_rotate  = 1;
  disp_drv.rotated    = LV_DISP_ROT_180;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchReadCB;
  lv_indev_drv_register(&indev_drv);

  // 2 ms LVGL tick from esp_timer (example 06's pattern).
  const esp_timer_create_args_t tick_args = { .callback = &lvglTickCB, .name = "lvgl_tick" };
  esp_timer_handle_t tick_timer;
  ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2 * 1000));

  PetState savedPet = storage.loadPet();
  pet.init(savedPet);

  PetSettings savedSettings = storage.loadSettings();
  pet.applySettings(savedSettings);

  storage.loadHabits(habits);
  if (habits.count() == 0) {
    habits.init();
  }

  ui.init(&pet, &habits);
  ui.setSoundCallback(petSoundCB);
  ui.setSleepCallback(sleepLogCB);
  ui.setPocketModeCallback(pocketModeCB);
  ui.setScreenPowerCallback(sitScreenPowerCB);
  ui.setRestartCallback(restartCB);
  ui.setSettingsCallback(settingsChangedCB);
  devCfg = storage.loadDeviceSettings();
  ui.setDeviceSettings(devCfg);           // also re-applies the pet background
  gfx->setBrightness(devCfg.brightness);  // splash used the 200 default until now
  ui.setBackDoneCallback(backDoneCB);
  backSessions = storage.loadBackSessions();
  wifiSync.setBackSessions(backSessions);
  ui.setPushDoneCallback(pushDoneCB);
  pushSessions = storage.loadPushSessions();
  wifiSync.setPushSessions(pushSessions);
  ui.setPullupDoneCallback(pullupDoneCB);
  pullupSessions = storage.loadPullupSessions();
  wifiSync.setPullupSessions(pullupSessions);
  ui.setFocusDoneCallback(focusDoneCB);
  focusSessions = storage.loadFocusSessions();
  wifiSync.setFocusSessions(focusSessions);
  // Restore the last-synced quest/goal lists from NVS so they don't
  // vanish on reboot while waiting for the next sync.
  {
    QuestInfo  cachedQuests[MAX_QUESTS];
    GoalInfo   cachedGoals[MAX_GOALS];
    TrophyInfo cachedTrophies[MAX_TROPHIES];
    int nq = storage.loadQuests(cachedQuests);
    int ng = storage.loadGoals(cachedGoals);
    int nt = storage.loadTrophies(cachedTrophies);
    ui.setQuests(cachedQuests, nq);
    ui.setGoals(cachedGoals, ng);
    ui.setTrophies(cachedTrophies, nt);
    lastTrophyCount = nt;  // celebration baseline (see applySyncResults)
  }

  // PCF85063 RTC — Wire is already running (started above for touch/PMU).
  timeKeeper.initRtc();

  // Walk app: restore today's step count. Rollover to a fresh day happens
  // in the loop poll once the clock reports a different calendar date.
  stepState = storage.loadStepState();
  ui.setSteps(stepState.steps, imuOk);
  wifiSync.setStepInfo(stepState.steps, stepState.year, stepState.dayOfYear);

  // Sleep app: restore the daily gate. Without a valid clock the gate stays
  // open (year 0 never matches); the loop check below corrects it once the
  // RTC/NTP comes up.
  sleepState = storage.loadSleepState();
  if (sleepState.year > 0)  // year 0 = never logged (or clock was unset)
    wifiSync.setSleepInfo(sleepState.quality, sleepState.year, sleepState.dayOfYear);
  if (timeKeeper.hasSync()) {
    WallDate t = timeKeeper.now();
    ui.setSleepLogged(t.year == sleepState.year && t.dayOfYear == sleepState.dayOfYear,
                      sleepState.quality);
  }

  storage.saveBootStage(3, wokeFromSleep);  // stage 3: display + UI up, WiFi next

  if (strlen(WIFI_SSID) > 0) {
    // Association kick only — begin() no longer blocks waiting for the link
    // (it used to hold the boot splash ~15 s). The loop's WiFi block already
    // finishes the online setup (NTP, SSE, OTA) once the link lands, same
    // path as recovering from a boot-time AP outage.
    wifiSync.begin(WIFI_SSID, WIFI_PASSWORD, DASHBOARD_URL);
  }

  lastResetYear = storage.loadLastResetYear();
  lastResetDOY  = storage.loadLastResetDay();
  lastXpResetToken = storage.loadXpResetToken();
  lastDailyCheck  = millis();
  lastSyncAttempt = millis();

  // First power-on/wake of the morning: open the sleep app so "how did I
  // sleep" gets answered while the answer is fresh. Only until it's logged
  // (the once-per-day gate) and only in the morning window — an afternoon
  // first boot isn't nagged. Every later boot lands on the pet as usual.
  if (timeKeeper.hasSync()) {
    WallDate t = timeKeeper.now();
    bool loggedToday = (t.year == sleepState.year && t.dayOfYear == sleepState.dayOfYear);
    if (!loggedToday && t.hour >= 4 && t.hour < 12) {
      Serial.println("morning boot - opening sleep app");
      ui.showSleepScreen();
    }
  }

  storage.saveBootStage(4, wokeFromSleep);  // stage 4: setup() complete
}

// After every successful sync: push results to the UI and cache the
// dashboard-owned lists in NVS (change-guarded) so they survive reboots.
static void applySyncResults() {
  ui.refreshHabitScreen();
  ui.setQuests(wifiSync.getQuests(), wifiSync.getQuestCount());
  ui.setGoals(wifiSync.getGoals(), wifiSync.getGoalCount());
  // Sync responses can change pet state directly (dashboard XP deltas,
  // resets) — without this the stage label/sprite freeze at their last
  // local-interaction values (seen live: device at blob LV.3, screen stuck
  // on "egg LV.0" after a reset+restore cycle).
  ui.refreshPetScreen();
  ui.setTrophies(wifiSync.getTrophies(), wifiSync.getTrophyCount());
  // New trophy? Celebrate: gold pill + fanfare. Count can also DROP (the
  // server recomputes live, e.g. after an XP reset) — that's silent.
  if (wifiSync.getTrophyCount() > lastTrophyCount) {
    ui.showTrophyPill();
    petSoundCB(SOUND_TROPHY);
  }
  lastTrophyCount = wifiSync.getTrophyCount();
  storage.saveHabits(habits);
  storage.saveQuests(wifiSync.getQuests(), wifiSync.getQuestCount());
  storage.saveGoals(wifiSync.getGoals(), wifiSync.getGoalCount());
  storage.saveTrophies(wifiSync.getTrophies(), wifiSync.getTrophyCount());

  // One-shot XP reset from the dashboard (Options tab): applied exactly once
  // per token, remembered across reboots.
  if (wifiSync.getPetResetToken() > lastXpResetToken) {
    lastXpResetToken = wifiSync.getPetResetToken();
    pet.resetProgress();
    storage.savePet(pet.getState());
    storage.saveXpResetToken(lastXpResetToken);
    ui.refreshPetScreen();
  }
  lastSyncAttempt = millis();
}

static void fireDailyReset() {
  int done   = habits.doneTodayCount();  // both counted before resetDaily
  int missed = habits.missedToday();     // clears the flags
  pet.dailyTick(done, missed);
  habits.resetDaily();
  storage.savePet(pet.getState());
  storage.saveHabits(habits);
  ui.refreshPetScreen();
}

void loop() {
  // Boot forensics: stage 0 = this boot made it out of setup() and into the
  // main loop. A later abnormal reset then reads as a runtime crash, not a
  // boot-path one.
  static bool loopStamped = false;
  if (!loopStamped) {
    loopStamped = true;
    storage.saveBootStage(0, wokeFromSleep);
  }

  // LVGL runs here — no display task on this board.
  lv_timer_handler();

  // ── One-time NTP → RTC stamp ─────────────────────────────────────────────
  // Once NTP first syncs, write the local time into the PCF85063 so future
  // standalone boots have accurate wall-clock resets without WiFi.
  static bool rtcSyncedFromNtp = false;
  if (ntpEnabled && !rtcSyncedFromNtp) {
    if (timeKeeper.syncRtcFromNtp()) {
      rtcSyncedFromNtp = true;
    }
  }

  // ── Daily reset ───────────────────────────────────────────────────────────
  int resetHour = pet.getSettings().dailyResetHour;

  if (timeKeeper.hasSync()) {
    if (lastResetDOY == -1) {
      WallDate today = timeKeeper.now();
      if (today.valid) {
        lastResetYear = today.year;
        lastResetDOY  = today.dayOfYear;
        storage.saveLastResetYear(lastResetYear);
        storage.saveLastResetDay(lastResetDOY);
      }
    } else if (timeKeeper.shouldReset(lastResetYear, lastResetDOY, resetHour)) {
      WallDate today = timeKeeper.now();
      fireDailyReset();
      if (today.year != lastResetYear) {
        storage.saveLastResetYear(today.year);
        lastResetYear = today.year;
      }
      if (today.dayOfYear != lastResetDOY) {
        storage.saveLastResetDay(today.dayOfYear);
        lastResetDOY = today.dayOfYear;
      }
    }
  } else {
    if (millis() - lastDailyCheck >= DAY_MS) {
      lastDailyCheck = millis();
      fireDailyReset();
    }
  }

  // ── Change-guarded NVS save ───────────────────────────────────────────────
  static uint32_t lastSaveCheck = 0;
  static PetState    lastSavedPet      = {};
  static Habit       lastSavedHabits[MAX_HABITS] = {};
  static PetSettings lastSavedSettings = DEFAULT_PET_SETTINGS;
  if (millis() - lastSaveCheck > 5000) {
    lastSaveCheck = millis();
    PetState current = pet.getState();
    if (memcmp(&current, &lastSavedPet, sizeof(PetState)) != 0) {
      storage.savePet(current);
      lastSavedPet = current;
    }
    if (memcmp(habits.habits, lastSavedHabits, sizeof(lastSavedHabits)) != 0) {
      storage.saveHabits(habits);
      memcpy(lastSavedHabits, habits.habits, sizeof(lastSavedHabits));
    }
    PetSettings curSettings = pet.getSettings();
    if (memcmp(&curSettings, &lastSavedSettings, sizeof(PetSettings)) != 0) {
      storage.saveSettings(curSettings);
      lastSavedSettings = curSettings;
    }
  }

  // ── Manual sync (press & hold on the pet screen) ────────────────────────
  // ui.syncStarted() already showed the spinner; note that with LVGL pumped
  // from loop(), the spinner freezes while sync() blocks on HTTP.
  if (ui.consumeSyncRequest()) {
    // sync() tries the USB bridge first, then WiFi; works USB-only too.
    bool ok = wifiSync.sync(&pet, &habits);
    if (ok) applySyncResults();
    ui.syncFinished(ok);
  }

  // ── WiFi sync ─────────────────────────────────────────────────────────────
  if (strlen(WIFI_SSID) > 0) {
    static uint32_t lastSseConnectAttempt = 0;
    static uint32_t lastWifiKick = 0;
    bool wifiUp = wifiSync.isConnected();

    // Self-healing: if the AP wasn't up at boot (frequent with this router)
    // or the link dropped and auto-reconnect gave up, kick a fresh
    // association every 3 min. Non-blocking; isConnected() sees the result
    // on later passes.
    if (!wifiUp && millis() - lastWifiKick > 180000UL) {
      lastWifiKick = millis();
      Serial.println("WiFi: link down - retrying association");
      wifiSync.kickReconnect();
    }

    if (wifiUp) {
      startOtaIfNeeded();
      ArduinoOTA.handle();
    }

    // One-time online setup that setup() skipped if boot was offline.
    if (wifiUp && !ntpEnabled) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      setenv("TZ", TIMEZONE, 1);
      tzset();
      ntpEnabled = true;
      Serial.println("WiFi: connected after boot - NTP configured");
    }

    if (wifiSync.isEventStreamConnected()) {
      if (wifiSync.pollEventStream(&pet, &habits)) applySyncResults();
    } else if (wifiUp) {  // don't hammer HTTP/SSE attempts with no link
      if (millis() - lastConfigCheck > CONFIG_CHECK_MS) {
        lastConfigCheck = millis();
        if (wifiSync.checkConfig(&pet, &habits)) applySyncResults();
      }
      // 15 s between attempts (was 5): with the 1 s connect cap that bounds
      // SSE's worst-case loop-blocking to ~7% when the host is unreachable.
      if (millis() - lastSseConnectAttempt > 15000) {
        lastSseConnectAttempt = millis();
        wifiSync.beginEventStream();
      }
    }

  }

  // Full sync — every 10 s while a USB host is attached (cheap local bridge),
  // every 60 s over WiFi (sync() itself prefers the USB bridge over WiFi HTTP).
  uint32_t syncInterval = wifiSync.usbAvailable() ? SYNC_INTERVAL_USB_MS : SYNC_INTERVAL_MS;
  if ((strlen(WIFI_SSID) > 0 || wifiSync.usbAvailable()) &&
      millis() - lastSyncAttempt > syncInterval) {
    lastSyncAttempt = millis();
    if (wifiSync.sync(&pet, &habits)) {
      applySyncResults();
      lastSyncOkMs = millis();
    }
  }

  // ── WiFi status line (settings screen) ────────────────────────────────────
  // "Is it connected?" at a glance: link + IP on the first line, dashboard
  // reachability on the second (live SSE, last successful sync age, or none).
  {
    static uint32_t lastWifiStatusPush = 0;
    if (millis() - lastWifiStatusPush > 3000) {
      lastWifiStatusPush = millis();
      char text[80];
      bool up = wifiSync.isConnected();
      char dash[40];
      if (wifiSync.isEventStreamConnected())
        snprintf(dash, sizeof(dash), "dashboard live");
      else if (lastSyncOkMs != 0)
        snprintf(dash, sizeof(dash), "synced %lus ago",
                 (unsigned long)((millis() - lastSyncOkMs) / 1000));
      else
        snprintf(dash, sizeof(dash), "no sync yet");
      if (strlen(WIFI_SSID) == 0)
        snprintf(text, sizeof(text), "WiFi off (no credentials)\n%s", dash);
      else if (up)
        snprintf(text, sizeof(text), "%s  %s\n%s", WIFI_SSID,
                 WiFi.localIP().toString().c_str(), dash);
      else
        snprintf(text, sizeof(text), "%s  not connected\n%s", WIFI_SSID, dash);
      ui.setWifiStatus(up, text);
    }
  }

  // ── Heap telemetry (visible in the USB bridge log) ────────────────────────
  {
    static uint32_t lastHeapLog = 0;
    if (millis() - lastHeapLog > 30000) {
      lastHeapLog = millis();
      lv_mem_monitor_t m;
      lv_mem_monitor(&m);
      Serial.printf("heap: free %u, min-ever %u, sw %u hw %u, lvmem free %u frag %u%%\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)swSteps,
                    (unsigned)(imuOk ? qmi.getPedometerCounter() : 0),
                    (unsigned)m.free_size, (unsigned)m.frag_pct);
      // Repeat the crash record every ~5 min: a wake-crash on battery has
      // already lost its boot lines by the time the device gets docked, and
      // docking doesn't reboot — this is how the record reaches the bridge.
      static uint8_t crashRecordTick = 0;
      if (++crashRecordTick >= 10) {
        crashRecordTick = 0;
        printCrashRecord();
      }
    }
  }

  // ── Walk app: software step detection + bookkeeping ───────────────────────
  // devCfg.stepsOn (settings screen) pauses the background ~66 Hz accel reads
  // and the 2 s poll to save battery; today's count freezes and resumes on
  // re-enable. The sampler still runs during focus/back sessions because it
  // doubles as their motion detector (guilt-trip / rep counter) — those are
  // short and user-initiated, so they don't drain anything in the background.
  if (imuOk && (devCfg.stepsOn || ui.isFocusRunning() || ui.isBackRunning()))
    swStepSample();   // self-throttles to ~66 Hz

  static uint32_t lastStepPoll = 0, lastStepPersist = 0;
  if (imuOk && devCfg.stepsOn && millis() - lastStepPoll > 2000) {
    lastStepPoll = millis();

    // New calendar day (RTC/NTP) → today's count starts over.
    if (timeKeeper.hasSync()) {
      WallDate today = timeKeeper.now();
      if (today.year != stepState.year || today.dayOfYear != stepState.dayOfYear) {
        stepState.year      = today.year;
        stepState.dayOfYear = today.dayOfYear;
        stepState.steps     = 0;
        stepState.rewarded  = false;
        storage.saveStepState(stepState);
        ui.setSteps(0, true);
      }
    }

    // Software counter drives the app; hardware counter logged alongside in
    // case the silicon pedometer ever comes to life (then it can take over).
    uint32_t raw = swSteps;
    static uint32_t lastRawLogged = 0;
    if (raw != lastRawLogged) {
      lastRawLogged = raw;
      Serial.printf("step: sw %u hw %u today %u\n", (unsigned)raw,
                    (unsigned)qmi.getPedometerCounter(),
                    (unsigned)(stepState.steps + (raw > pedLastRaw ? raw - pedLastRaw : 0)));
    }
    if (raw < pedLastRaw) pedLastRaw = 0;  // counter reset (reboot)
    if (raw > pedLastRaw) {
      stepState.steps += raw - pedLastRaw;
      pedLastRaw = raw;
      ui.setSteps(stepState.steps, true);
      wifiSync.setStepInfo(stepState.steps, stepState.year, stepState.dayOfYear);

      if (!stepState.rewarded && stepState.steps >= WALK_DAILY_GOAL) {
        stepState.rewarded = true;
        pet.addXP(WALK_GOAL_XP);
        ui.refreshPetScreen();
        storage.savePet(pet.getState());
        storage.saveStepState(stepState);
        Serial.printf("walk goal reached: +%d xp\n", WALK_GOAL_XP);
      }
    }

    // Persist at most once a minute; saveStepState skips unchanged writes.
    if (millis() - lastStepPersist > 60000) {
      lastStepPersist = millis();
      storage.saveStepState(stepState);
    }
  }

  // ── Pet-screen clock ──────────────────────────────────────────────────────
  // Cheap: timeKeeper caches RTC reads for 10 s, and the label only redraws
  // when the text actually changes (LVGL no-ops identical set_text).
  {
    static uint32_t lastClockPush = 0;
    if (millis() - lastClockPush > 5000 && timeKeeper.hasSync()) {
      lastClockPush = millis();
      WallDate t = timeKeeper.now();
      ui.updateClock(t.hour, t.minute);
    }
  }

  // ── Hunger decay, wall-clock driven ───────────────────────────────────────
  // hungerHourlyTick() fires per elapsed RTC hour, with the last applied hour
  // kept in NVS — so a night of deep sleep or a day powered off is charged in
  // one catch-up burst at the next boot. (Before July 2026 nothing on this
  // board called the tick; the blob was permanently full and nobody had to
  // work out. Catch-up is capped at 48 h so a long shelf stay leaves the pet
  // starving-but-rescuable rather than instantly dead.)
  {
    static uint32_t lastHungerCheck = 0;
    if (millis() - lastHungerCheck > 60000 && timeKeeper.hasSync() && pet.isAlive()) {
      lastHungerCheck = millis();
      WallDate t = timeKeeper.now();
      uint32_t hourIndex = ((uint32_t)t.year * 366UL + (uint32_t)t.dayOfYear) * 24UL + t.hour;
      uint32_t last = storage.loadHungerClock();
      if (last == 0 || last > hourIndex) {
        storage.saveHungerClock(hourIndex);  // first run (or clock went backwards)
      } else if (hourIndex > last) {
        uint32_t elapsed = hourIndex - last;
        if (elapsed > 48) elapsed = 48;
        for (uint32_t i = 0; i < elapsed; i++) pet.hungerHourlyTick();
        storage.saveHungerClock(hourIndex);
        storage.savePet(pet.getState());
        ui.refreshPetScreen();
        Serial.printf("hunger: -%lu h of decay applied, now %d\n",
                      (unsigned long)elapsed, pet.getHunger());
      }
    }
  }

  // ── Sleep app: keep the once-per-day gate honest ──────────────────────────
  // Re-arms the question on a new calendar day, and closes the gate if the
  // clock only became valid after boot (the setup() restore couldn't tell).
  {
    static uint32_t lastSleepCheck = 0;
    if (millis() - lastSleepCheck > 60000 && timeKeeper.hasSync()) {
      lastSleepCheck = millis();
      WallDate t = timeKeeper.now();
      ui.setSleepLogged(t.year == sleepState.year && t.dayOfYear == sleepState.dayOfYear,
                        sleepState.quality);
    }
  }

  // ── Workout motion capture lifecycle (pull-ups + back) ────────────────────
  // Arm the PSRAM recorder when a session starts; when it ends, ship the
  // capture to the dashboard's Motion Lab. WiFi may still be self-healing
  // right after a workout, so retry every 20 s and give up after 15 min (a
  // pending capture also inhibits auto-sleep below, briefly).
  static bool     capPending      = false;
  static uint32_t capPendingSince = 0;
  {
    static bool        wasCapRunning = false;
    static uint32_t    capLastTry    = 0;
    static const char* capLabel      = "pullup";
    bool running = ui.isPullupRunning() || ui.isBackRunning();
    if (running && !wasCapRunning) {
      if (!capBuf) capBuf = (uint16_t*)ps_malloc(CAP_MAX * sizeof(uint16_t));
      capLen = 0;
      capPending = false;
      capLabel = ui.isBackRunning() ? "back" : "pullup";
    } else if (!running && wasCapRunning && capLen > 0) {
      capPending = true;
      capPendingSince = millis();
      capLastTry = 0;
      Serial.printf("motionlog: captured %d samples (%s), uploading when WiFi allows\n",
                    capLen, capLabel);
    }
    wasCapRunning = running;

    if (capPending && millis() - capPendingSince > 15UL * 60UL * 1000UL) {
      capPending = false;  // WiFi never came back; don't hold sleep hostage
      Serial.println("motionlog: gave up waiting for WiFi, capture dropped");
    }
    if (capPending && wifiSync.isConnected() && millis() - capLastTry > 20000UL) {
      capLastTry = millis();
      if (wifiSync.postMotionLog(capLabel, capBuf, capLen, 66)) capPending = false;
    }
  }

  // ── Auto-sleep: a left-on device dims, then deep-sleeps ──────────────────
  // Inhibited while USB-powered (docked = bridge sync), in pocket mode (step
  // counting), during focus/back sessions, or while steps keep arriving
  // (walking with the screen on never generates LVGL input activity).
  {
    static uint32_t lastIdleCheck = 0;
    static bool     dimmed        = false;
    if (millis() - lastIdleCheck > 1000) {
      lastIdleCheck = millis();
      uint32_t idle = lv_disp_get_inactive_time(NULL);
      uint32_t sleepMs = (uint32_t)devCfg.sleepMin * 60000UL;
      uint32_t dimMs   = sleepMs * 3 / 4;
      bool inhibited = pocketMode || ui.isFocusRunning() || ui.isBackRunning() ||
                       ui.isPullupRunning() || ui.isCleanRunning() ||
                       ui.isHangRunning() ||  // a hold is 2 untouched minutes; don't dim mid-set
                       ui.isSitRunning() ||  // a deep-sleeping device can't nag you to stand
                       ui.isMedRunning() ||  // meditation darkens its own screen; sleep would kill the timer
                       capPending ||
                       (pmuOk && power.isVbusIn()) ||
                       (swLastStepMs != 0 && millis() - swLastStepMs < sleepMs);
      if (inhibited || idle < dimMs) {
        if (dimmed && !pocketMode) { dimmed = false; gfx->setBrightness(devCfg.brightness); }
      } else if (idle >= sleepMs) {
        enterAutoSleep();  // never returns
      } else if (!dimmed) {
        dimmed = true;
        // warning dim; any touch restores. Scales with the user's brightness
        // so a dim-preferring user still sees a visible step down.
        uint8_t dim = devCfg.brightness / 4;
        gfx->setBrightness(dim < 15 ? 15 : dim);
      }
    }
  }

  // ── BOOT button: short press = apps menu, hold 2 s = power off ────────────
  // Runtime GPIO0 is a free input (its strapping role only matters at reset).
  // Power-off saves state, blanks the panel, then tells the PMU to drop all
  // rails; power back on with the PWR button or by plugging in USB.
  {
    static uint32_t bootHeldSince = 0;
    if (digitalRead(0) == LOW) {
      if (bootHeldSince == 0) {
        bootHeldSince = millis();
      } else if (pmuOk && millis() - bootHeldSince > 2000) {
        Serial.println("BOOT held - powering off");
        storage.savePet(pet.getState());
        storage.saveHabits(habits);
        storage.saveStepState(stepState);
        gfx->setBrightness(0);
        power.shutdown();  // never returns
      }
    } else if (bootHeldSince != 0) {
      uint32_t held = millis() - bootHeldSince;
      bootHeldSince = 0;
      // 50 ms floor debounces contact bounce; releases past 2 s only happen
      // without a PMU (shutdown never returns), so treat those as holds too.
      if (held >= 50 && held < 2000) {
        lv_disp_trig_activity(NULL);  // button use counts against the auto-sleep idle clock
        if (pocketMode) {
          // Wake from pocket mode straight back into the walk screen.
          pocketMode = false;
          gfx->setBrightness(devCfg.brightness);
          ui.showWalkScreen();
          Serial.println("pocket mode OFF");
        } else if (sitDisplayOff) {
          // Sit mode with the screen dark: BOOT just relights it.
          sitScreenPowerCB(true);
        } else if (ui.isPushRunning()) {
          // Push-up session: BOOT is the DONE button (no on-screen DONE —
          // mid-push-up the nose counter made it a mis-tap hazard).
          ui.finishPushSession();
        } else if (ui.isSquatRunning()) {
          // Squat session: same contract — taps are reps, BOOT is DONE.
          ui.finishSquatSession();
        } else {
          ui.showAppsMenu();
        }
      }
    }
  }

  // ── Battery status (AXP2101 fuel gauge) ───────────────────────────────────
  static uint32_t lastBattCheck = 0;
  static const uint32_t BATT_CHECK_MS = 60UL * 1000UL;
  if (pmuOk && millis() - lastBattCheck > BATT_CHECK_MS) {
    lastBattCheck = millis();
    int pct = power.getBatteryPercent();      // -1 when no battery attached
    bool charging = power.isCharging();
    if (pct >= 0) ui.updateBattery(pct, charging);
  }

  delay(5);
}
