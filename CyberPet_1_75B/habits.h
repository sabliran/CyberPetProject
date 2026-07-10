#pragma once
#include <Arduino.h>

// Device-side habit capacity. Habits beyond this simply don't fit and are
// silently skipped by sync reconciliation — raise it if the dashboard list
// grows. Storage::loadHabits accepts saves of any older (smaller) capacity.
#define MAX_HABITS 16
#define HABIT_NAME_LEN 24

struct Habit {
  char name[HABIT_NAME_LEN];
  int xpValue;
  bool doneToday;
  int streak;
  bool active;    // false = empty slot
  int serverId;   // dashboard habit.id; -1 = no server id yet (local / unsynced)
};

// Streak multiplier: XP bonus percent by current streak length. Tiers (not a
// per-day ramp) so the small base XP values (5–15) still round to visible
// bonuses, and each tier is a milestone to protect (loss aversion: breaking
// a 30-day streak drops you from double XP back to earn-it-again).
// MUST stay deterministic from `streak` alone — habit un-complete recomputes
// the exact awarded amount to revert it (see ui.cpp habitButtonEventCB).
inline int streakBonusPercent(int streak) {
  if (streak >= 30) return 100;  // 2.0x
  if (streak >= 14) return 75;   // 1.75x
  if (streak >= 7)  return 50;   // 1.5x
  if (streak >= 3)  return 25;   // 1.25x
  return 0;
}

// XP actually awarded for completing a habit at a given streak (streak value
// AFTER the completion's increment). Shared by the award and the undo path.
inline int habitAwardXP(int xpValue, int streak) {
  return xpValue + xpValue * streakBonusPercent(streak) / 100;
}

class HabitTracker {
public:
  HabitTracker();

  void init();

  // Returns habit index, or -1 if list full.
  // serverId: pass the dashboard habit.id when known; -1 for local/seeded habits.
  int addHabit(const char* name, int xpValue, int serverId = -1);
  void removeHabit(int index);

  bool completeHabit(int index);   // returns false if already done today
  bool uncompleteHabit(int index); // undo today's completion; returns false if not done today
  bool anyDoneToday() const;

  // Call once per day at RTC-driven midnight rollover
  void resetDaily();

  Habit* get(int index);
  int count() const { return habitCount; }

  // Recalculate habitCount from active flags. MUST be called after habits[]
  // is restored from raw storage bytes (see Storage::loadHabits), since
  // habitCount is not part of the persisted array.
  void recount();

  Habit habits[MAX_HABITS];

private:
  int habitCount;
};
