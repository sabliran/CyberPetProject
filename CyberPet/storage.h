#pragma once
#include <Preferences.h>
#include "pet.h"
#include "habits.h"

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

private:
  Preferences prefs;
};
