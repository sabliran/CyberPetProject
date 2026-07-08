#pragma once
#include <Arduino.h>

// Tunable behaviour received from the dashboard via sync.
// Defaults match the values that were previously hardcoded in pet.cpp.
// Persisted to NVS so a standalone boot after a previous WiFi sync keeps
// the last-configured values instead of falling back to the compile-time
// defaults.  applySettings() validates each field before storing.
struct PetSettings {
  int moodGainPerHabit;  // mood boost when any habit is done    (daily tick)
  int moodDecayPerMiss;  // mood loss  when no habit is done     (daily tick)
  int dailyResetHour;    // local hour at which habits roll over (0-23)
};

// These match the server's DEFAULT_DATA.settings and pet.cpp's old literals.
// dailyResetHour also mirrors DEFAULT_DAILY_RESET_HOUR in timekeeping.h (both = 4).
static const PetSettings DEFAULT_PET_SETTINGS = { 2, 15, 4 };

// Evolution stages — blob visually grows and gains glow as it evolves
enum PetStage {
  STAGE_EGG = 0,
  STAGE_BLOB,
  STAGE_CREATURE,
  STAGE_EVOLVED,
  STAGE_COUNT
};

// Workout difficulty — determines rep target required to earn one feeding
enum WorkoutDifficulty { DIFF_EASY = 0, DIFF_MEDIUM, DIFF_HARD };
static const int WORKOUT_TARGETS[3] = { 15, 40, 80 };

// Level at which each stage begins (index = stage).
// Level formula: xpForLevel(L) = 500 * L*(L+1)/2
//   Level  0 :       0 XP  → Egg
//   Level  2 :    1500 XP  → Blob
//   Level  5 :    7500 XP  → Creature
//   Level  8 :   18000 XP  → Evolved
static const int STAGE_LEVEL_THRESHOLDS[STAGE_COUNT] = {0, 2, 5, 8};

struct PetState {
  int xp;
  int stage;
  int mood;          // 0-100, drops if habits neglected
  int daysAlive;
  uint32_t lastCareTimestamp;
  int hunger;        // 0-100; drops 25/day without feeding; 0 then another day unfed = dead
  bool fedToday;     // reset to false each daily tick
  bool alive;        // false = pet died from starvation
  int dashXpApplied; // lifetime dashboard XP already credited; delta vs server total is idempotent
};

class Pet {
public:
  Pet();

  void init(const PetState& loaded);
  PetState getState() const { return state; }

  void addXP(int amount);
  void dailyTick(bool anyHabitDoneToday);
  void feed();           // call when workout target is met; resets hunger, boosts mood

  int getStage()   const { return state.stage; }
  int getXP()      const { return state.xp; }
  int getMood()    const;   // effective mood — capped by hunger
  int getHunger()  const { return state.hunger; }
  bool isFedToday() const { return state.fedToday; }
  bool isAlive()   const { return state.alive; }
  const char* getStageName() const;

  // Level system — all derived from total XP, nothing extra stored
  int getLevel()           const;
  int xpToNextLevel()      const; // XP still needed to reach next level
  int xpIntoCurrentLevel() const; // XP earned since current level started

  // Total XP required to reach level L from 0 (public so dev menu can query)
  static int xpForLevel(int level);

  // Apply XP awarded on the dashboard (quests, manual completions).
  // total = server's lifetime dashXpTotal; only the delta above dashXpApplied
  // is credited, so calling this multiple times with the same total is safe.
  void applyDashboardXpTotal(int total);

  // Dashboard settings — applied via applySettings(); persisted to NVS in
  // CyberPet.ino's change-guarded save block so a standalone boot after a
  // previous WiFi sync keeps the last-configured values.
  void applySettings(const PetSettings& s); // validates ranges before storing
  PetSettings getSettings() const;

  // Dev / test helpers (also useful for NVS restore path on firmware)
  void setXP(int totalXP);
  void setMood(int mood);
  void setHunger(int hunger);  // dev only — clamps 0-100, also resets alive if > 0

private:
  PetState    state;
  PetSettings settings;
  void checkEvolution();
};
