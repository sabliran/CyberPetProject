#pragma once
#include <Arduino.h>

#define MAX_HABITS 8
#define HABIT_NAME_LEN 24

struct Habit {
  char name[HABIT_NAME_LEN];
  int xpValue;
  bool doneToday;
  int streak;
  bool active;    // false = empty slot
  int serverId;   // dashboard habit.id; -1 = no server id yet (local / unsynced)
};

class HabitTracker {
public:
  HabitTracker();

  void init();

  // Returns habit index, or -1 if list full.
  // serverId: pass the dashboard habit.id when known; -1 for local/seeded habits.
  int addHabit(const char* name, int xpValue, int serverId = -1);
  void removeHabit(int index);

  bool completeHabit(int index);   // returns false if already done today
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
