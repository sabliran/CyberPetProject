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

// --- Fill these in, or leave WIFI_SSID empty to run fully standalone ---
const char* WIFI_SSID     = "REDACTED_WIFI_SSID";
const char* WIFI_PASSWORD = "REDACTED_WIFI_PASSWORD";
// *.local hostnames are resolved via mDNS after WiFi connects, so this stays
// valid when the host's DHCP address changes.
const char* DASHBOARD_URL = "http://omarchy.local:8090";

// POSIX TZ string.  "UTC0" fires daily reset at UTC midnight.
// Example: "CET-1CEST,M3.5.0,M10.5.0/3" for Central Europe.
const char* TIMEZONE = "UTC0";

// ── Board drivers (from Waveshare's 1.75 example 06) ────────────────────────
static Arduino_DataBus* bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
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
  // before the next one counts (hysteresis). Cadence gate: steps closer
  // than 250 ms apart are jitter, not walking.
  if (!swAbove && swFilt > 0.06f) {
    swAbove = true;
    if (nowMs - swLastStepMs > 250) {
      swSteps++;
      swLastStepMs = nowMs;
    }
  } else if (swAbove && swFilt < 0.02f) {
    swAbove = false;
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

// Registered with the UI layer (see PetSoundEvent in ui.h).
static void petSoundCB(int event) {
  if (!audioOk) return;
  switch (event) {
    case SOUND_HABIT_DONE:    // cheerful up-chirp (A5 -> E6)
      playTone(880.0f, 70, 0.5f);
      playTone(1318.5f, 120, 0.5f);
      break;
    case SOUND_HABIT_UNDONE:  // single low blip
      playTone(523.3f, 90, 0.35f);
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
static uint32_t lastDailyCheck = 0;
static const uint32_t DAY_MS   = 24UL * 60UL * 60UL * 1000UL;

uint32_t lastSyncAttempt  = 0;
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

static void touchReadCB(lv_indev_drv_t* indev, lv_indev_data_t* data) {
  // The CST92xx pulses TP_INT on every scan while touched, including a final
  // report with zero points on release — same read pattern as Waveshare's demo.
  static int16_t tx[5], ty[5];
  if (touchPressed) {
    uint8_t touched = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (touched) {
      touchPressed = false;
      data->state   = LV_INDEV_STATE_PRESSED;
      data->point.x = tx[0];
      data->point.y = ty[0];
    } else {
      data->state = LV_INDEV_STATE_RELEASED;
    }
  }
}

static void lvglTickCB(void* arg) { lv_tick_inc(2); }

void setup() {
  // USB sync bridge sends ~1 KB responses in one burst; the HWCDC default
  // RX buffer (256 B) would overflow. Must be set before Serial.begin().
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);

  // Crash forensics: the USB bridge log keeps this line, so an unexpected
  // reboot names its cause even when the panic text is lost with the port.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("boot: reset reason %d (%s)\n", (int)rr,
        rr == ESP_RST_POWERON  ? "power-on"      :
        rr == ESP_RST_SW       ? "software"       :
        rr == ESP_RST_PANIC    ? "panic"          :
        rr == ESP_RST_INT_WDT  ? "int watchdog"   :
        rr == ESP_RST_TASK_WDT ? "task watchdog"  :
        rr == ESP_RST_WDT      ? "other watchdog" :
        rr == ESP_RST_BROWNOUT ? "brownout"       : "other");
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
    touch.setMirrorXY(true, true);  // panel mounted rotated (example 06)
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

  gfx->begin();
  gfx->setBrightness(200);

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

  storage.begin();

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
  // Restore the last-synced quest/goal lists from NVS so they don't
  // vanish on reboot while waiting for the next sync.
  {
    QuestInfo cachedQuests[MAX_QUESTS];
    GoalInfo  cachedGoals[MAX_GOALS];
    int nq = storage.loadQuests(cachedQuests);
    int ng = storage.loadGoals(cachedGoals);
    ui.setQuests(cachedQuests, nq);
    ui.setGoals(cachedGoals, ng);
  }

  // PCF85063 RTC — Wire is already running (started above for touch/PMU).
  timeKeeper.initRtc();

  // Walk app: restore today's step count. Rollover to a fresh day happens
  // in the loop poll once the clock reports a different calendar date.
  stepState = storage.loadStepState();
  ui.setSteps(stepState.steps, imuOk);
  wifiSync.setStepInfo(stepState.steps, stepState.year, stepState.dayOfYear);

  if (strlen(WIFI_SSID) > 0) {
    wifiSync.begin(WIFI_SSID, WIFI_PASSWORD, DASHBOARD_URL);
    if (wifiSync.isConnected()) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      setenv("TZ", TIMEZONE, 1);
      tzset();
      ntpEnabled = true;
      wifiSync.beginEventStream();
    }
  }

  lastResetYear = storage.loadLastResetYear();
  lastResetDOY  = storage.loadLastResetDay();
  lastXpResetToken = storage.loadXpResetToken();
  lastDailyCheck  = millis();
  lastSyncAttempt = millis();
}

// After every successful sync: push results to the UI and cache the
// dashboard-owned lists in NVS (change-guarded) so they survive reboots.
static void applySyncResults() {
  ui.refreshHabitScreen();
  ui.setQuests(wifiSync.getQuests(), wifiSync.getQuestCount());
  ui.setGoals(wifiSync.getGoals(), wifiSync.getGoalCount());
  storage.saveHabits(habits);
  storage.saveQuests(wifiSync.getQuests(), wifiSync.getQuestCount());
  storage.saveGoals(wifiSync.getGoals(), wifiSync.getGoalCount());

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
  bool didAnything = habits.anyDoneToday();
  pet.dailyTick(didAnything);
  habits.resetDaily();
  storage.savePet(pet.getState());
  storage.saveHabits(habits);
  ui.refreshPetScreen();
}

void loop() {
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

    if (wifiSync.isEventStreamConnected()) {
      if (wifiSync.pollEventStream(&pet, &habits)) applySyncResults();
    } else {
      if (millis() - lastConfigCheck > CONFIG_CHECK_MS) {
        lastConfigCheck = millis();
        if (wifiSync.checkConfig(&pet, &habits)) applySyncResults();
      }
      if (millis() - lastSseConnectAttempt > 5000) {
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
    if (wifiSync.sync(&pet, &habits)) applySyncResults();
  }

  // ── Heap telemetry (visible in the USB bridge log) ────────────────────────
  {
    static uint32_t lastHeapLog = 0;
    if (millis() - lastHeapLog > 30000) {
      lastHeapLog = millis();
      Serial.printf("heap: free %u, min-ever %u, sw %u hw %u\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)swSteps,
                    (unsigned)(imuOk ? qmi.getPedometerCounter() : 0));
    }
  }

  // ── Walk app: software step detection + bookkeeping ───────────────────────
  if (imuOk) swStepSample();   // every loop pass; self-throttles to ~66 Hz

  static uint32_t lastStepPoll = 0, lastStepPersist = 0;
  if (imuOk && millis() - lastStepPoll > 2000) {
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
      if (held >= 50 && held < 2000) ui.showAppsMenu();
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
