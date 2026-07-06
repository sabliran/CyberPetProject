#include "pet.h"

// Total XP required to reach level L from level 0.
// Each level-up costs 500*(L) XP, so the cumulative is a triangular number:
//   L=0:0  L=1:500  L=2:1500  L=3:3000  L=4:5000  L=5:7500  ...
int Pet::xpForLevel(int level) {
  return 500 * level * (level + 1) / 2;
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
  state.hunger  = 100;
  state.fedToday = false;
  state.alive   = true;
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

void Pet::feed() {
  if (!state.alive) return;
  state.hunger   = 100;
  state.fedToday = true;
  state.mood     = min(100, state.mood + 15);
}

void Pet::dailyTick(bool anyHabitDoneToday) {
  state.daysAlive++;

  // Hunger decay — if already at 0 and still not fed, the pet dies
  if (!state.fedToday) {
    if (state.hunger == 0) {
      state.alive = false;
    } else {
      state.hunger = max(0, state.hunger - 25);
    }
  }
  state.fedToday = false;  // reset for the new day

  if (!state.alive) {
    state.mood = 0;
    checkEvolution();
    return;
  }

  // Habit mood adjustment
  if (!anyHabitDoneToday) {
    state.mood = max(0, state.mood - 15);
  } else {
    state.mood = min(100, state.mood + 2);
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
  // Walk the stage thresholds backwards to find highest unlocked stage
  for (int s = STAGE_COUNT - 1; s >= 0; s--) {
    if (lv >= STAGE_LEVEL_THRESHOLDS[s]) {
      state.stage = s;
      return;
    }
  }
  state.stage = STAGE_EGG;
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

const char* Pet::getStageName() const {
  switch (state.stage) {
    case STAGE_EGG:      return "Egg";
    case STAGE_BLOB:     return "Blob";
    case STAGE_CREATURE: return "Creature";
    case STAGE_EVOLVED:  return "Evolved";
    default:             return "???";
  }
}
