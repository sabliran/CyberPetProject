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
  } else if (len == sizeof(PetState) - sizeof(int)) {
    // Pre-health save (July 2026): `health` was appended as the last field,
    // so the old bytes map onto the struct front unchanged. Migrate instead
    // of discarding — a size-mismatch fallthrough would wipe XP and streaks.
    prefs.getBytes("pet_state", &state, len);
    state.health = 100;
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
    state.health        = 100;
  }
  return state;
}

void Storage::saveHabits(HabitTracker &tracker) {
  // Key "habits_v2": bumped from "habits" when Habit gained the serverId field.
  // Old saves (sizeof mismatch) are silently discarded by loadHabits.
  prefs.putBytes("habits_v2", tracker.habits, sizeof(tracker.habits));
  prefs.putInt("habit_count", tracker.count());
}

void Storage::loadHabits(HabitTracker &tracker) {
  size_t len = prefs.getBytesLength("habits_v2");
  // Accept any whole number of habit slots up to capacity, not just an exact
  // array match — a save written when MAX_HABITS was smaller (e.g. 8 → 16)
  // loads into the first slots and the rest stay inactive. A Habit struct
  // layout change still mismatches (len % sizeof != 0) and is discarded.
  if (len > 0 && len <= sizeof(tracker.habits) && len % sizeof(Habit) == 0) {
    prefs.getBytes("habits_v2", tracker.habits, len);
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

void Storage::saveReminders(const ReminderInfo* list, int count) {
  if (count < 0) count = 0;
  if (count > MAX_REMINDERS) count = MAX_REMINDERS;
  // Change-guard: sync runs every 10 s over USB; don't rewrite identical bytes.
  ReminderInfo cur[MAX_REMINDERS];
  if (loadReminders(cur) == count &&
      memcmp(cur, list, sizeof(ReminderInfo) * count) == 0) return;
  if (count > 0) prefs.putBytes("reminders", list, sizeof(ReminderInfo) * count);
  else           prefs.remove("reminders");
  prefs.putInt("remind_count", count);
}

int Storage::loadReminders(ReminderInfo* list) {
  int count = prefs.getInt("remind_count", 0);
  if (count <= 0 || count > MAX_REMINDERS) return 0;
  size_t len = prefs.getBytesLength("reminders");
  if (len != sizeof(ReminderInfo) * (size_t)count) return 0;  // struct changed / stale
  prefs.getBytes("reminders", list, len);
  return count;
}

void Storage::saveReminderDays(const int16_t days[MAX_REMINDERS]) {
  int16_t cur[MAX_REMINDERS];
  loadReminderDays(cur);
  if (memcmp(cur, days, sizeof cur) == 0) return;
  prefs.putBytes("remind_days", days, sizeof(int16_t) * MAX_REMINDERS);
}

void Storage::loadReminderDays(int16_t days[MAX_REMINDERS]) {
  for (int i = 0; i < MAX_REMINDERS; i++) days[i] = -1;
  size_t len = prefs.getBytesLength("remind_days");
  if (len == sizeof(int16_t) * MAX_REMINDERS) prefs.getBytes("remind_days", days, len);
}

void Storage::saveLastResetYear(int year) {
  prefs.putInt("last_reset_yr", year);
}

int Storage::loadLastResetYear() {
  return prefs.getInt("last_reset_yr", -1);
}

void Storage::saveSettings(const PetSettings& s) {
  prefs.putBytes("pet_settings", &s, sizeof(PetSettings));
}

PetSettings Storage::loadSettings() {
  PetSettings s = DEFAULT_PET_SETTINGS;
  size_t len = prefs.getBytesLength("pet_settings");
  if (len == sizeof(PetSettings)) {
    prefs.getBytes("pet_settings", &s, sizeof(PetSettings));
  } else if (len == sizeof(PetSettings) - sizeof(int)) {
    // Pre-difficulty save (July 2026): the field was appended last, so the
    // old bytes fill the struct front; difficulty keeps its default (normal).
    prefs.getBytes("pet_settings", &s, len);
    s.difficulty = 1;
  }
  return s;
}

void Storage::saveQuests(const QuestInfo* quests, int count) {
  if (count < 0) count = 0;
  if (count > MAX_QUESTS) count = MAX_QUESTS;
  // Change-guard: sync runs every 10 s over USB; don't rewrite identical bytes.
  QuestInfo cur[MAX_QUESTS];
  if (loadQuests(cur) == count &&
      memcmp(cur, quests, sizeof(QuestInfo) * count) == 0) return;
  if (count > 0) prefs.putBytes("quests", quests, sizeof(QuestInfo) * count);
  else           prefs.remove("quests");
  prefs.putInt("quest_count", count);
}

int Storage::loadQuests(QuestInfo* quests) {
  int count = prefs.getInt("quest_count", 0);
  if (count <= 0 || count > MAX_QUESTS) return 0;
  size_t len = prefs.getBytesLength("quests");
  if (len != sizeof(QuestInfo) * (size_t)count) return 0;  // struct changed / stale
  prefs.getBytes("quests", quests, len);
  return count;
}

void Storage::saveGoals(const GoalInfo* goals, int count) {
  if (count < 0) count = 0;
  if (count > MAX_GOALS) count = MAX_GOALS;
  GoalInfo cur[MAX_GOALS];
  if (loadGoals(cur) == count &&
      memcmp(cur, goals, sizeof(GoalInfo) * count) == 0) return;
  if (count > 0) prefs.putBytes("goals", goals, sizeof(GoalInfo) * count);
  else           prefs.remove("goals");
  prefs.putInt("goal_count", count);
}

int Storage::loadGoals(GoalInfo* goals) {
  int count = prefs.getInt("goal_count", 0);
  if (count <= 0 || count > MAX_GOALS) return 0;
  size_t len = prefs.getBytesLength("goals");
  if (len != sizeof(GoalInfo) * (size_t)count) return 0;  // struct changed / stale
  prefs.getBytes("goals", goals, len);
  return count;
}

void Storage::saveXpResetToken(int token) {
  prefs.putInt("xp_reset_tok", token);
}

int Storage::loadXpResetToken() {
  return prefs.getInt("xp_reset_tok", 0);
}

void Storage::saveStepState(const StepState& s) {
  StepState cur = loadStepState();
  if (memcmp(&cur, &s, sizeof(StepState)) == 0) return;  // skip no-op flash writes
  prefs.putBytes("steps", &s, sizeof(StepState));
}

StepState Storage::loadStepState() {
  StepState s = {};
  size_t len = prefs.getBytesLength("steps");
  if (len == sizeof(StepState)) prefs.getBytes("steps", &s, sizeof(StepState));
  return s;
}

void Storage::savePlankState(const PlankState& s) {
  PlankState cur = loadPlankState();
  if (memcmp(&cur, &s, sizeof(PlankState)) == 0) return;
  prefs.putBytes("plank", &s, sizeof(PlankState));
}

PlankState Storage::loadPlankState() {
  PlankState s = {};
  size_t len = prefs.getBytesLength("plank");
  if (len == sizeof(PlankState)) prefs.getBytes("plank", &s, sizeof(PlankState));
  return s;
}

void Storage::saveHangState(const HangState& s) {
  HangState cur = loadHangState();
  if (memcmp(&cur, &s, sizeof(HangState)) == 0) return;
  prefs.putBytes("hang", &s, sizeof(HangState));
}

HangState Storage::loadHangState() {
  HangState s = {};
  size_t len = prefs.getBytesLength("hang");
  if (len == sizeof(HangState)) prefs.getBytes("hang", &s, sizeof(HangState));
  return s;
}

void Storage::saveSleepState(const SleepState& s) {
  SleepState cur = loadSleepState();
  if (memcmp(&cur, &s, sizeof(SleepState)) == 0) return;
  prefs.putBytes("sleep", &s, sizeof(SleepState));
}

SleepState Storage::loadSleepState() {
  SleepState s = {};
  size_t len = prefs.getBytesLength("sleep");
  if (len == sizeof(SleepState)) prefs.getBytes("sleep", &s, sizeof(SleepState));
  return s;
}

void Storage::saveTrophies(const TrophyInfo* trophies, int count) {
  if (count < 0) count = 0;
  if (count > MAX_TROPHIES) count = MAX_TROPHIES;
  TrophyInfo cur[MAX_TROPHIES];
  if (loadTrophies(cur) == count &&
      memcmp(cur, trophies, sizeof(TrophyInfo) * count) == 0) return;
  if (count > 0) prefs.putBytes("trophies", trophies, sizeof(TrophyInfo) * count);
  else           prefs.remove("trophies");
  prefs.putInt("trophy_count", count);
}

int Storage::loadTrophies(TrophyInfo* trophies) {
  int count = prefs.getInt("trophy_count", 0);
  if (count <= 0 || count > MAX_TROPHIES) return 0;
  size_t len = prefs.getBytesLength("trophies");
  if (len != sizeof(TrophyInfo) * (size_t)count) return 0;  // struct changed / stale
  prefs.getBytes("trophies", trophies, len);
  return count;
}

void Storage::saveBackSessions(uint32_t n) {
  if (loadBackSessions() == n) return;
  prefs.putUInt("back_sessions", n);
}

uint32_t Storage::loadBackSessions() {
  return prefs.getUInt("back_sessions", 0);
}

void Storage::savePushSessions(uint32_t n) {
  if (loadPushSessions() == n) return;
  prefs.putUInt("push_sessions", n);
}

uint32_t Storage::loadPushSessions() {
  return prefs.getUInt("push_sessions", 0);
}

void Storage::savePullupSessions(uint32_t n) {
  if (loadPullupSessions() == n) return;
  prefs.putUInt("pull_sessions", n);
}

uint32_t Storage::loadPullupSessions() {
  return prefs.getUInt("pull_sessions", 0);
}

void Storage::saveHungerClock(uint32_t hourIndex) {
  if (loadHungerClock() == hourIndex) return;
  prefs.putUInt("hunger_hour", hourIndex);
}

uint32_t Storage::loadHungerClock() {
  return prefs.getUInt("hunger_hour", 0);
}

void Storage::saveDeviceSettings(const DeviceSettings& s) {
  DeviceSettings cur = loadDeviceSettings();
  if (memcmp(&cur, &s, sizeof(DeviceSettings)) == 0) return;
  prefs.putBytes("dev_settings", &s, sizeof(DeviceSettings));
}

DeviceSettings Storage::loadDeviceSettings() {
  DeviceSettings s = { 100, 0, 200, 0, 2, 1, 1 };  // defaults mirror PetUI::init()
  size_t len = prefs.getBytesLength("dev_settings");
  if (len == sizeof(DeviceSettings)) {
    prefs.getBytes("dev_settings", &s, sizeof(DeviceSettings));
  } else if (len > 0 && len < sizeof(DeviceSettings)) {
    // Blob written by firmware before the struct grew. Fields are append-only
    // plain bytes, so a partial read keeps the user's old knobs and the new
    // trailing fields stay at their defaults.
    prefs.getBytes("dev_settings", &s, len);
  }
  return s;
}

void Storage::saveBootStage(uint8_t stage, bool wasWake) {
  // No change-guard needed: NVS itself skips writes when the value is
  // unchanged, and this runs a handful of times per boot.
  prefs.putUChar("boot_stage", stage);
  prefs.putUChar("boot_wake", wasWake ? 1 : 0);
}

uint8_t Storage::loadBootStage(bool* wasWake) {
  if (wasWake) *wasWake = prefs.getUChar("boot_wake", 0) != 0;
  return prefs.getUChar("boot_stage", 0);
}

void Storage::saveBootAnomaly(const BootAnomaly& a) {
  prefs.putBytes("boot_anom", &a, sizeof(BootAnomaly));
}

BootAnomaly Storage::loadBootAnomaly() {
  BootAnomaly a = {};
  if (prefs.getBytesLength("boot_anom") == sizeof(BootAnomaly))
    prefs.getBytes("boot_anom", &a, sizeof(BootAnomaly));
  return a;
}

void Storage::saveFocusSessions(uint32_t n) {
  if (loadFocusSessions() == n) return;
  prefs.putUInt("focus_sessions", n);
}

uint32_t Storage::loadFocusSessions() {
  return prefs.getUInt("focus_sessions", 0);
}
