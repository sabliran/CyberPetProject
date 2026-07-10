#include "pet.h"

// Total XP required to reach level L from level 0.
// Level L costs 50*L + 50 XP, so the cumulative is quadratic:
//   L=0:0  L=1:100  L=2:250  L=3:450  L=6:1350  L=12:4500  ...
// Cheap early levels give a first-day level-up; the +50/level ramp keeps
// later levels meaningful without stalling. Keep server.js stageFromXP()
// identical.
int Pet::xpForLevel(int level) {
  return 25 * level * level + 75 * level;
}

static int levelFromXP(int xp) {
  int lv = 0;
  while (Pet::xpForLevel(lv + 1) <= xp) lv++;
  return lv;
}

Pet::Pet() {
  state.xp    = 0;
  state.stage = STAGE_EGG;
  state.mood  = 80;
  state.daysAlive          = 0;
  state.lastCareTimestamp  = 0;
  state.hunger       = 100;
  state.fedToday     = false;
  state.alive        = true;
  state.dashXpApplied = 0;
  settings = DEFAULT_PET_SETTINGS;
}

void Pet::init(const PetState& loaded) {
  state = loaded;
  checkEvolution(); // ensure stage is consistent with loaded XP
}

void Pet::addXP(int amount) {
  state.xp += amount;
  state.mood = min(100, state.mood + 8);
  checkEvolution();
}

void Pet::removeXP(int amount) {
  state.xp   = max(0, state.xp - amount);
  state.mood = max(0, state.mood - 8);
  checkEvolution();  // stage derives from XP, so this also demotes if needed
}

void Pet::resetProgress() {
  state.xp            = 0;
  state.dashXpApplied = 0;  // server zeroes dashXpTotal in the same reset
  checkEvolution();         // back to egg
}

void Pet::feed(int hungerAmount, int moodBoost) {
  if (!state.alive) return;
  state.hunger   = min(100, state.hunger + max(0, hungerAmount));
  state.fedToday = true;
  state.mood     = min(100, state.mood + max(0, moodBoost));
}

void Pet::hungerHourlyTick() {
  if (!state.alive) return;
  state.hunger = max(0, state.hunger - HUNGER_DECAY_PER_HOUR);
}

void Pet::dailyTick(bool anyHabitDoneToday) {
  state.daysAlive++;

  // Hunger now decays continuously via hungerHourlyTick(); the daily tick
  // only enforces the death rule: a full unfed day at 0 hunger is fatal.
  if (!state.fedToday && state.hunger == 0) {
    state.alive = false;
  }
  state.fedToday = false;  // reset for the new day

  if (!state.alive) {
    state.mood = 0;
    checkEvolution();
    return;
  }

  // Habit mood adjustment
  if (!anyHabitDoneToday) {
    state.mood = max(0, state.mood - settings.moodDecayPerMiss);
  } else {
    state.mood = min(100, state.mood + settings.moodGainPerHabit);
  }

  // Hunger penalises mood: low hunger = big mood decay
  if (state.hunger < 20) {
    state.mood = max(0, state.mood - 20);
  } else if (state.hunger < 50) {
    state.mood = max(0, state.mood - 10);
  }

  checkEvolution();
}

void Pet::checkEvolution() {
  int lv = levelFromXP(state.xp);
  int xpStage = STAGE_EGG;
  for (int s = STAGE_COUNT - 1; s >= 0; s--) {
    if (lv >= STAGE_LEVEL_THRESHOLDS[s]) { xpStage = s; break; }
  }
  // Low mood demotes displayed stage by one (recoverable — XP is never touched).
  // Only applies while alive; death has its own visual state.
  if (state.alive && state.mood < MOOD_REGRESSION_THRESHOLD && xpStage > STAGE_EGG) {
    state.stage = xpStage - 1;
  } else {
    state.stage = xpStage;
  }
}

int Pet::getMood() const {
  if (!state.alive)      return 0;
  if (state.hunger < 20) return max(0, min(state.mood, 15));   // starving  → mood hard-capped at 15
  if (state.hunger < 50) return max(0, min(state.mood, 55));   // very hungry → capped at 55
  return state.mood;
}

int Pet::getLevel() const {
  return levelFromXP(state.xp);
}

int Pet::xpIntoCurrentLevel() const {
  return state.xp - xpForLevel(getLevel());
}

int Pet::xpToNextLevel() const {
  int lv = getLevel();
  return xpForLevel(lv + 1) - state.xp;
}

void Pet::setXP(int totalXP) {
  state.xp = max(0, totalXP);
  checkEvolution();
}

void Pet::setMood(int mood) {
  state.mood = max(0, min(100, mood));
}

void Pet::setHunger(int hunger) {
  state.hunger = max(0, min(100, hunger));
  if (state.hunger > 0) state.alive = true;
}

void Pet::applySettings(const PetSettings& s) {
  // Validate each field independently — a bad payload must not zero out mood
  // mechanics (mirrors the server's /api/settings whitelist philosophy).
  if (s.moodGainPerHabit >= 0 && s.moodGainPerHabit <= 100)
    settings.moodGainPerHabit = s.moodGainPerHabit;
  if (s.moodDecayPerMiss >= 0 && s.moodDecayPerMiss <= 100)
    settings.moodDecayPerMiss = s.moodDecayPerMiss;
  if (s.dailyResetHour >= 0 && s.dailyResetHour <= 23)
    settings.dailyResetHour = s.dailyResetHour;
}

PetSettings Pet::getSettings() const { return settings; }

void Pet::applyDashboardXpTotal(int total) {
  if (total > state.dashXpApplied) {
    addXP(total - state.dashXpApplied);
    state.dashXpApplied = total;
  }
}

const char* Pet::getStageName() const {
  switch (state.stage) {
    case STAGE_EGG:      return "Egg";
    case STAGE_BLOB:     return "Blob";
    case STAGE_CREATURE: return "Creature";
    case STAGE_EVOLVED:  return "Evolved";
    default:             return "???";
  }
}
