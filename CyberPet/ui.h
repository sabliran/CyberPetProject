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

private:
  Pet* pet;
  HabitTracker* tracker;

  lv_obj_t* petScreen;
  lv_obj_t* habitScreen;

  // pet screen widgets
  lv_obj_t* blobShape;
  lv_obj_t* eyeLeft;   // black dot, direct child of blobShape
  lv_obj_t* eyeRight;
  lv_obj_t* stageLabel;
  lv_obj_t* moodBar;
  lv_obj_t* xpLabel;

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

  // pet screen — hunger + mood indicators
  lv_obj_t*  hungerLabel;
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

  void buildPetScreen();
  void buildHabitScreen();
  void buildWorkoutScreen();
  void startBlobAnimations();
  void roamToRandom();
  void applyMoodExpression();
  void showXpPopup(lv_area_t coords, int xpValue);

  static void exprTimerCB(lv_timer_t* t);
  static void revertExprCB(lv_timer_t* t);
  static void roamTimerCB(lv_timer_t* t);
  static void blinkTimerCB(lv_timer_t* t);
  static void habitButtonEventCB(lv_event_t* e);
  static void goToHabitsEventCB(lv_event_t* e);
  static void goToPetEventCB(lv_event_t* e);
  static void workoutGoToCB(lv_event_t* e);
  static void workoutTapCB(lv_event_t* e);
  static void workoutStartBtnCB(lv_event_t* e);
  static void workoutDoneBtnCB(lv_event_t* e);
  static void workoutBackBtnCB(lv_event_t* e);
  static void workoutClockCB(lv_timer_t* t);
  static void workoutDiffBtnCB(lv_event_t* e);
};
