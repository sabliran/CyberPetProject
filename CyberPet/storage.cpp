#include "storage.h"

void Storage::begin() {
  // namespace "cyberpet", read/write mode
  prefs.begin("cyberpet", false);
}

void Storage::savePet(const PetState &state) {
  prefs.putBytes("pet_state", &state, sizeof(PetState));
}

PetState Storage::loadPet() {
  PetState state;
  size_t len = prefs.getBytesLength("pet_state");
  if (len == sizeof(PetState)) {
    prefs.getBytes("pet_state", &state, sizeof(PetState));
  } else {
    // no saved state yet (or struct size changed) — use defaults
    state.xp = 0;
    state.stage = STAGE_EGG;
    state.mood = 80;
    state.daysAlive = 0;
    state.lastCareTimestamp = 0;
    state.hunger        = 100;
    state.fedToday      = false;
    state.alive         = true;
    state.dashXpApplied = 0;
  }
  return state;
}

void Storage::saveHabits(HabitTracker &tracker) {
  prefs.putBytes("habits", tracker.habits, sizeof(tracker.habits));
  prefs.putInt("habit_count", tracker.count());
}

void Storage::loadHabits(HabitTracker &tracker) {
  size_t len = prefs.getBytesLength("habits");
  if (len == sizeof(tracker.habits)) {
    prefs.getBytes("habits", tracker.habits, sizeof(tracker.habits));
  }
  // habitCount is NOT stored in the byte blob - recount from active flags,
  // otherwise count() returns 0 after a reboot and init() would append
  // duplicate default habits into the free slots.
  tracker.recount();
}

void Storage::saveLastResetDay(int dayOfYear) {
  prefs.putInt("last_reset_day", dayOfYear);
}

int Storage::loadLastResetDay() {
  return prefs.getInt("last_reset_day", -1);
}
