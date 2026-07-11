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

  // Last-applied dashboard XP-reset token (monotonic; see WifiSync).
  void saveXpResetToken(int token);
  int  loadXpResetToken();

  // Walk app daily steps (change-guarded like quests/goals; the sketch polls
  // the pedometer every couple of seconds but only persists on change).
  void saveStepState(const StepState& s);
  StepState loadStepState();

private:
  Preferences prefs;
};
