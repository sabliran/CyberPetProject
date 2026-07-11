#pragma once
#include <lvgl.h>
#include "pet.h"
#include "habits.h"

// Written against LVGL v8.2+ API. Requirements/caveats:
// - lv_obj_set_user_data / lv_obj_get_user_data (used in ui.cpp to stash the
//   habit index on each list button) exist from v8.2 onward. On older v8.0/8.1
//   assign obj->user_data directly instead.
// - If Waveshare's example ships LVGL v9, a handful of calls need mechanical
//   renames (lv_obj_set_style_* signatures, lv_btn_* -> lv_button_*).
// Check the LVGL version in Waveshare's 02_Example project before compiling.

// Quests are dashboard-owned and read-only on the device: the sync response's
// `quests` array (active, not-done) is copied here for display on the quest
// screen. Completing a quest happens on the dashboard.
#define MAX_QUESTS     8
#define QUEST_NAME_LEN 40

struct QuestInfo {
  char name[QUEST_NAME_LEN];
  int  xp;
};

// Goals are dashboard-owned and read-only on the device, same as quests:
// the sync response's `goals` array (active only) is copied here for display
// on the goal screen. Completing a goal happens on the dashboard.
#define MAX_GOALS       8
#define GOAL_NAME_LEN   40
#define GOAL_PERIOD_LEN 12

struct GoalInfo {
  char name[GOAL_NAME_LEN];
  int  xp;
  char period[GOAL_PERIOD_LEN];  // "daily" / "weekly" / ... (free-form from dashboard)
};

// Sound events. The UI layer only *emits* these; the board sketch registers a
// player via setSoundCallback() (speaker/codec wiring is board-specific).
// Unset callback = silent, so the sim and speakerless boards need nothing.
enum PetSoundEvent {
  SOUND_HABIT_DONE = 0,
  SOUND_HABIT_UNDONE,
};
typedef void (*PetSoundCB)(int event);

class PetUI {
public:
  // Call once after lvgl + display + touch are already initialized
  // (i.e. after the Waveshare example's own display/touch init code runs).
  void init(Pet* pet, HabitTracker* tracker);

  void refreshPetScreen();
  void refreshHabitScreen();
  void showPetScreen();
  void showHabitScreen();
  void showWorkoutScreen();
  void startIdleWobble(); // called by roamArrivalCB (file-scope)

  // Called from main_web.cpp on spacebar; no-op unless workout is running
  void addWorkoutRep();
  bool isWorkoutRunning() const { return workoutRunning; }

  void showPomodoroScreen();
  void pomodoroGuiltTrip();   // hook for IMU "picked up during focus" guilt-trip
  void sedentaryNudge();       // trigger sedentary state (also called by sim `s` key)
  void updateBattery(int pct, bool charging = false); // pct 0-100; -1 = unknown; call periodically from loop()

  // Quest screen (pet screen swipe-right). Data comes from the dashboard sync
  // response; call setQuests after every successful sync.
  void showQuestScreen();
  void setQuests(const QuestInfo* quests, int count);

  // Goal screen (quest screen swipe-right — the carousel continues:
  // pet > quests > goals, swipe left to walk back). Same sync contract as
  // quests: call setGoals after every successful sync.
  void showGoalScreen();
  void setGoals(const GoalInfo* goals, int count);

  // Manual sync: press & hold anywhere on the pet screen (LVGL long-press,
  // ~400 ms) sets a request flag and shows
  // the "syncing..." overlay. loop() polls consumeSyncRequest(), runs the
  // blocking sync (the FreeRTOS LVGL task keeps the spinner animating), then
  // reports the outcome via syncFinished().
  bool consumeSyncRequest();
  void syncStarted();            // shows spinner overlay (no-op if already shown)
  void syncFinished(bool ok);    // green "synced" / red "offline", then fades out

  // Apps menu: a launcher listing the built-in apps (workout, focus, ...).
  // The board sketch wires it to a physical button (1.75B: short-press BOOT);
  // the sim maps it to the 'a' key. Calling it while the menu is already
  // showing closes it (toggle), so one button both opens and dismisses.
  void showAppsMenu();

  // Register after init(). See PetSoundEvent above.
  void setSoundCallback(PetSoundCB cb) { soundCB = cb; }

private:
  Pet* pet;
  HabitTracker* tracker;
  PetSoundCB soundCB = nullptr;  // deliberately NOT reset in init()

  lv_obj_t* petScreen;
  lv_obj_t* habitScreen;

  // pet screen widgets
  lv_obj_t* blobShape;
  lv_obj_t* eyeLeft;   // black dot, direct child of blobShape
  lv_obj_t* eyeRight;
  lv_obj_t* stageLabel;
  lv_obj_t* moodBar;
  lv_obj_t* xpLabel;

  // battery state
  lv_obj_t*    batteryLabel;      // pet screen indicator; hidden until first update
  int          batteryPct;        // -1 = unknown/not yet read
  bool         batteryWarnShown;  // true once "getting sleepy" bubble has fired this low episode

  // sedentary nudge state
  bool         sedentaryActive;   // true while blob is in droopy/restless state
  lv_timer_t*  sedentaryTimer;    // polls lv_disp_get_inactive_time every 60 s

  // animation & expression state
  lv_coord_t blobBaseX, blobBaseY;   // last roam target; -1 = uninitialized
  lv_coord_t eyeOffX, eyeOffY;
  lv_coord_t eyeDriftX, eyeDriftY;   // added to eye position while roaming
  int        eyeBaseW;
  lv_timer_t* exprTimer;
  lv_timer_t* roamTimer;
  lv_timer_t* blinkTimer;

  // habit screen widgets
  lv_obj_t* habitList;

  // quest screen widgets + data
  lv_obj_t*  questScreen;
  lv_obj_t*  questList;
  QuestInfo  quests[MAX_QUESTS];
  int        questCount;

  // goal screen widgets + data
  lv_obj_t*  goalScreen;
  lv_obj_t*  goalList;
  GoalInfo   goals[MAX_GOALS];
  int        goalCount;

  // apps menu screen
  lv_obj_t*  appsScreen;

  // manual-sync state (press & hold + overlay)
  uint32_t   lastGestureMs;  // swipes also emit clicks on release; used to filter them out
  uint32_t   lastPatMs;      // debounce for the blob pat reaction
  bool       syncRequested;
  lv_obj_t*  syncOverlay;      // nullptr when not showing
  lv_obj_t*  syncLabel;
  lv_obj_t*  syncSpinner;
  lv_timer_t* syncTimeoutTimer; // safety: auto-dismiss if loop never reports back

  // pet screen — hunger + mood indicators
  lv_obj_t*  hungerLabel;
  lv_obj_t*  hungerBar;
  lv_obj_t*  moodLabel;

  // workout screen widgets + state
  lv_obj_t*  workoutScreen;
  lv_obj_t*  repLabel;
  lv_obj_t*  timerLabel;
  lv_obj_t*  workoutHint;
  lv_obj_t*  workoutStartLabel;
  lv_obj_t*  workoutDoneLabel;
  lv_obj_t*  diffBtns[3];          // Easy / Medium / Hard selector
  int        workoutReps;
  bool       workoutRunning;
  WorkoutDifficulty workoutDifficulty;
  uint32_t   workoutStartMs;
  uint32_t   workoutElapsedMs;
  lv_timer_t* workoutClockTimer;

  // pomodoro screen widgets + state
  lv_obj_t*  pomodoroScreen;
  lv_obj_t*  pomArc;
  lv_obj_t*  pomTimeLabel;
  lv_obj_t*  pomModeLabel;
  lv_obj_t*  pomBlockLabel;
  lv_obj_t*  pomStartLabel;
  lv_timer_t* pomClockTimer;

  enum PomState { POM_IDLE, POM_FOCUS, POM_BREAK } pomState;
  uint32_t   pomLastTapMs;   // double-tap detection for play/pause
  bool       pomRunning;
  int        pomBlocks;      // focus blocks completed this session
  uint32_t   pomStartMs;     // lv_tick_get() when current interval started
  uint32_t   pomElapsedMs;   // accumulated ms before last pause

  void buildPetScreen();
  void buildHabitScreen();
  void buildQuestScreen();
  void refreshQuestScreen();
  void buildGoalScreen();
  void refreshGoalScreen();
  void buildAppsScreen();
  void buildWorkoutScreen();
  void buildPomodoroScreen();
  void refreshPomodoroRing();
  void startBlobAnimations();
  void roamToRandom();
  void applyMoodExpression();
  void showXpPopup(lv_area_t coords, int xpValue);
  void showEvolutionBurst();
  void showLevelUpPill(int level);   // "LEVEL N!" celebration (no stage change)
  void showSedentaryBubble();
  void showSleepyBubble();
  void blobLoveReaction();   // pat response: happy eyes, zoom pulse, floating heart

  static void pomClockCB(lv_timer_t* t);
  static void pomStartBtnCB(lv_event_t* e);
  static void pomCancelBtnCB(lv_event_t* e);
  static void pomGuiltRevertCB(lv_timer_t* t);
  static void sedentaryCheckCB(lv_timer_t* t);

  static void exprTimerCB(lv_timer_t* t);
  static void revertExprCB(lv_timer_t* t);
  static void roamTimerCB(lv_timer_t* t);
  static void blinkTimerCB(lv_timer_t* t);
  static void habitButtonEventCB(lv_event_t* e);
  // Gesture navigation: pet screen is the hub. Swipe left = habits,
  // up = workout, down = focus; the opposite swipe exits back to the pet.
  static void petGestureCB(lv_event_t* e);
  static void habitGestureCB(lv_event_t* e);
  static void questGestureCB(lv_event_t* e);
  static void goalGestureCB(lv_event_t* e);
  static void workoutGestureCB(lv_event_t* e);
  static void pomGestureCB(lv_event_t* e);
  static void appsGestureCB(lv_event_t* e);
  static void appsBtnCB(lv_event_t* e);
  static void petLongPressCB(lv_event_t* e);    // press & hold = manual sync
  static void blobPatCB(lv_event_t* e);         // quick tap on the blob = love reaction
  static void pomTapCB(lv_event_t* e);          // double-tap = play/pause on focus screen
  static void syncTimeoutCB(lv_timer_t* t);
  static void syncOverlayDeleteCB(lv_anim_t* a);
  static void workoutTapCB(lv_event_t* e);
  static void workoutStartBtnCB(lv_event_t* e);
  static void workoutDoneBtnCB(lv_event_t* e);
  static void workoutBackBtnCB(lv_event_t* e);
  static void workoutClockCB(lv_timer_t* t);
  static void workoutDiffBtnCB(lv_event_t* e);
};
