#pragma once
#include <Preferences.h>
#include "pet.h"
#include "habits.h"
#include "ui.h"   // QuestInfo / GoalInfo (dashboard-owned lists, cached for reboot)

// Daily step-counter state (walk app). The hardware pedometer count resets at
// power-off, so the sketch accumulates into `steps` and persists it here;
// year/dayOfYear stamp which wall date the count belongs to (midnight
// rollover), and `rewarded` stops the daily goal XP from double-awarding
// across reboots. Zero-init (= {}) matters: the change-guard memcmp in
// saveStepState compares padding bytes too.
struct StepState {
  int      year;       // 0 = never stamped (count predates first valid clock)
  int      dayOfYear;
  uint32_t steps;
  bool     rewarded;
};

// Sleep app: which calendar date last night's rating was logged on (the
// once-per-day gate) and what it was. Zero-init (= {}) matters for the
// change-guard memcmp, same as StepState.
struct SleepState {
  int year;      // 0 = never logged / logged with an unset clock
  int dayOfYear;
  int quality;   // 0 good, 1 medium, 2 bad
};

// Record of the last abnormal reset (panic / watchdog / brownout), written at
// the boot that follows the crash. `reason` holds the esp_reset_reason_t
// value; `stage` is the setup() breadcrumb the crashed boot left behind
// (0 = it had reached loop(), so the crash hit at runtime or during sleep);
// `wasWake` says whether the crashed boot was a deep-sleep wake.
struct BootAnomaly {
  uint8_t  reason;
  uint8_t  stage;
  uint8_t  wasWake;
  uint8_t  pad;      // keep the blob layout explicit
  uint32_t count;    // lifetime abnormal-reset total
};

// Wraps ESP32 NVS flash storage so pet + habit state survives reboots/power loss.
class Storage {
public:
  void begin();

  void savePet(const PetState &state);
  PetState loadPet();

  void saveHabits(HabitTracker &tracker);
  void loadHabits(HabitTracker &tracker);

  void saveLastResetDay(int dayOfYear);
  int  loadLastResetDay();

  void saveLastResetYear(int year);
  int  loadLastResetYear();

  void saveSettings(const PetSettings& s);
  PetSettings loadSettings();

  // Dashboard-owned read-only lists (quests/goals): cached in NVS so they
  // survive a reboot instead of vanishing until the next sync. save* skips
  // the flash write when the data is unchanged (sync runs every 10 s on USB).
  // load* fills the caller's array (must hold MAX_QUESTS / MAX_GOALS) and
  // returns the count.
  void saveQuests(const QuestInfo* quests, int count);
  int  loadQuests(QuestInfo* quests);
  void saveGoals(const GoalInfo* goals, int count);
  int  loadGoals(GoalInfo* goals);
  void saveTrophies(const TrophyInfo* trophies, int count);
  int  loadTrophies(TrophyInfo* trophies);

  // Last-applied dashboard XP-reset token (monotonic; see WifiSync).
  void saveXpResetToken(int token);
  int  loadXpResetToken();

  // Walk app daily steps (change-guarded like quests/goals; the sketch polls
  // the pedometer every couple of seconds but only persists on change).
  void saveStepState(const StepState& s);
  StepState loadStepState();

  // Sleep app daily rating gate.
  void saveSleepState(const SleepState& s);
  SleepState loadSleepState();

  // Back-workout app: lifetime completed-session count (trophy fuel).
  void saveBackSessions(uint32_t n);
  uint32_t loadBackSessions();

  // Push-up app: same contract.
  void savePushSessions(uint32_t n);
  uint32_t loadPushSessions();

  // Pull-up app: same contract.
  void savePullupSessions(uint32_t n);
  uint32_t loadPullupSessions();

  // Hunger decay clock: absolute wall-clock hour index of the last applied
  // decay tick, so hunger catches up across deep sleep and power-off.
  void saveHungerClock(uint32_t hourIndex);
  uint32_t loadHungerClock();

  // Device settings (swipe-up screen): volume, sounds, brightness, pet bg,
  // auto-sleep. Blob, change-guarded; load returns defaults when absent.
  void saveDeviceSettings(const DeviceSettings& s);
  DeviceSettings loadDeviceSettings();

  // Boot forensics. Deep-sleep wake crashes happen on battery, where serial
  // output has no listener — by the time the device is docked the panic text
  // is long gone. The sketch leaves a stage breadcrumb as setup() progresses
  // (0 = reached loop()), and after an abnormal reset records what killed the
  // previous boot so it can be replayed into the bridge log later.
  void saveBootStage(uint8_t stage, bool wasWake);
  uint8_t loadBootStage(bool* wasWake);
  void saveBootAnomaly(const BootAnomaly& a);
  BootAnomaly loadBootAnomaly();

  // Focus app: lifetime completed 25-min blocks.
  void saveFocusSessions(uint32_t n);
  uint32_t loadFocusSessions();

private:
  Preferences prefs;
};
