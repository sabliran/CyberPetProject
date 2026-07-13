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

// Feeding doesn't snap hunger to 100: each completed exercise session
// restores a fixed meal (back/pull-up apps call feed(45, 12)) against the
// continuous hourly decay — the pet literally runs on your exercise.
// (The tap-counting workout app with its Easy/Medium/Hard targets was
// removed July 2026 in favour of the IMU-counted apps.)

// Continuous hunger decay: applied per wall-clock hour by loop() / the sim,
// with NVS catch-up across sleep and power-off (storage.saveHungerClock).
// 3/hour ≈ 72/day: an unattended day visibly starves the blob, an overnight
// costs ~30, so mornings start hungry enough that a workout is truly needed.
// (Raised from 2 in July 2026 — and until then the 1.75B never called the
// tick at all, so hunger sat pinned at 100.)
static const int HUNGER_DECAY_PER_HOUR = 3;

// Mood below this threshold demotes the displayed stage by one (recoverable —
// XP is never touched; raise mood above this and the stage returns immediately).
// With default moodDecayPerMiss=15 this triggers after ~6 consecutive missed days.
static const int MOOD_REGRESSION_THRESHOLD = 20;

// Level at which each stage begins (index = stage).
// Level formula: xpForLevel(L) = 25*L*L + 75*L  (level L costs 50*L + 50 XP)
// Early levels are cheap on purpose — the first level-up should land on day
// one (fast onboarding win), then each level costs 50 XP more than the last:
//   Level  1 :     100 XP  (LV1 costs 100)
//   Level  2 :     250 XP  → Blob
//   Level  6 :    1350 XP  → Creature
//   Level 12 :    4500 XP  → Evolved
// Mirror any change in server.js stageFromXP() — the dashboard replicates
// this formula to update stage instantly on quest/goal completion.
static const int STAGE_LEVEL_THRESHOLDS[STAGE_COUNT] = {0, 2, 6, 12};

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
  void removeXP(int amount);  // exact inverse of addXP (habit un-done): xp and the +8 mood boost
  void resetProgress();       // dashboard-commanded: xp/stage/dashXpApplied to zero; mood/hunger/streaks untouched
  void dailyTick(bool anyHabitDoneToday);
  // Sleep app: 0 good, 1 medium, 2 bad. Good/medium lift mood + hunger;
  // bad costs XP, mood and hunger (see pet.cpp for the exact numbers).
  void logSleep(int quality);
  // Call when an exercise target is met. Restores `hungerAmount` (capped at
  // 100) and boosts mood by `moodBoost` (back/pull-up apps pass 45, 12).
  void feed(int hungerAmount = 55, int moodBoost = 15);
  // Continuous hunger decay — call once per real hour (loop()/sim timer).
  // Death still only happens at dailyTick (a full unfed day at 0 hunger).
  void hungerHourlyTick();

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
