#include "habits.h"
#include <string.h>

HabitTracker::HabitTracker() : habitCount(0) {
  for (int i = 0; i < MAX_HABITS; i++) {
    habits[i].active = false;
  }
}

void HabitTracker::init() {
  // Default starter habits - edit these or load from storage instead
  if (habitCount == 0) {
    addHabit("Drink water", 5);
    addHabit("Move body", 10);
    addHabit("Practice guitar", 10);
    addHabit("Read / study", 15);
  }
}

int HabitTracker::addHabit(const char* name, int xpValue, int serverId) {
  for (int i = 0; i < MAX_HABITS; i++) {
    if (!habits[i].active) {
      strncpy(habits[i].name, name, HABIT_NAME_LEN - 1);
      habits[i].name[HABIT_NAME_LEN - 1] = '\0';
      habits[i].xpValue   = xpValue;
      habits[i].doneToday = false;
      habits[i].streak    = 0;
      habits[i].active    = true;
      habits[i].serverId  = serverId;
      habitCount++;
      return i;
    }
  }
  return -1; // full
}

void HabitTracker::removeHabit(int index) {
  if (index < 0 || index >= MAX_HABITS) return;
  if (habits[index].active) {
    habits[index].active = false;
    habitCount--;
  }
}

bool HabitTracker::completeHabit(int index) {
  if (index < 0 || index >= MAX_HABITS) return false;
  Habit &h = habits[index];
  if (!h.active || h.doneToday) return false;
  h.doneToday = true;
  h.streak++;
  return true;
}

bool HabitTracker::uncompleteHabit(int index) {
  if (index < 0 || index >= MAX_HABITS) return false;
  Habit &h = habits[index];
  if (!h.active || !h.doneToday) return false;
  h.doneToday = false;
  if (h.streak > 0) h.streak--;  // undo the increment completeHabit made
  return true;
}

bool HabitTracker::anyDoneToday() const {
  for (int i = 0; i < MAX_HABITS; i++) {
    if (habits[i].active && habits[i].doneToday) return true;
  }
  return false;
}

int HabitTracker::missedToday() const {
  int missed = 0;
  for (int i = 0; i < MAX_HABITS; i++) {
    if (habits[i].active && !habits[i].doneToday) missed++;
  }
  return missed;
}

void HabitTracker::resetDaily() {
  for (int i = 0; i < MAX_HABITS; i++) {
    if (habits[i].active) {
      if (!habits[i].doneToday) {
        habits[i].streak = 0; // missed a day, streak breaks
      }
      habits[i].doneToday = false;
    }
  }
}

Habit* HabitTracker::get(int index) {
  if (index < 0 || index >= MAX_HABITS) return nullptr;
  return &habits[index];
}

void HabitTracker::recount() {
  habitCount = 0;
  for (int i = 0; i < MAX_HABITS; i++) {
    if (habits[i].active) habitCount++;
  }
}
