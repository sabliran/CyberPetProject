#include "ui.h"
#include <cstdlib>

// Stage colors - used for the evolution burst particles. The blob body itself
// is always pure white (stage shows through size + animation energy instead).
static lv_color_t stageColor(int stage) {
  switch (stage) {
    case STAGE_EGG:      return lv_color_hex(0xCFCFCF);
    case STAGE_BLOB:     return lv_color_hex(0x6FD08C);
    case STAGE_CREATURE: return lv_color_hex(0x4FA3E3);
    case STAGE_EVOLVED:  return lv_color_hex(0xC77DFF);
    default: return lv_color_hex(0xFFFFFF);
  }
}

static int stageSize(int stage) {
  switch (stage) {
    case STAGE_EGG:      return 80;
    case STAGE_BLOB:     return 110;
    case STAGE_CREATURE: return 140;
    case STAGE_EVOLVED:  return 170;
    default: return 100;
  }
}

// Per-stage animation parameters — more evolved = more energy
static const int BOUNCE_AMP_PX[]  = {4,   7,  11,  16};   // idle wobble amplitude
static const int BOUNCE_MS[]      = {900, 750, 600, 480};  // ms per wobble half
static const int ROAM_TRAVEL_MS[] = {1800, 1400, 1000, 700}; // ms to glide to new spot
static const int ROAM_PERIOD_MS[] = {5000, 3500, 2400, 1600}; // ms between new targets
// Glow: shadow_spread = solid white ring width (animated, clearly white)
//        shadow_width = tiny blur for soft outer edge only (keep small!)
// Large width + small spread = big grey gradient. Inverted: large spread + small width = white ring.
static const int GLOW_SPREAD[]    = {10,  22,  36,  50};  // base white ring width (px beyond edge)
static const int GLOW_PULSE[]     = {4,    8,  12,  16};  // ± breathing range (on spread)
static const int GLOW_PULSE_MS[]  = {2000, 1400, 1100, 850};
static const int GLOW_BASE_W[]    = {3,    4,   5,   6};  // small blur = soft outer edge
static const int EXPR_PERIOD_MS[] = {4000, 3000, 2200, 1600}; // expression cycle

static void makeCircle(lv_obj_t* obj, lv_color_t color) {
  lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(obj, color, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// LVGL animation exec callbacks
static void animSetX(void* obj, int32_t v) { lv_obj_set_x((lv_obj_t*)obj, v); }
static void animSetY(void* obj, int32_t v) { lv_obj_set_y((lv_obj_t*)obj, v); }
static void animSetW(void* obj, int32_t v) { lv_obj_set_width((lv_obj_t*)obj, v); }
static void animSetH(void* obj, int32_t v) { lv_obj_set_height((lv_obj_t*)obj, v); }
static void animSetGlow(void* obj, int32_t v) {
  lv_obj_set_style_shadow_spread((lv_obj_t*)obj, (lv_coord_t)v, 0);
}
static void animSetOpa(void* obj, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}
static void xpPopupDeleteCB(lv_anim_t* a) {
  // del_async avoids use-after-free: Y and Opa animations complete at the same tick,
  // so deleting pill synchronously in Y's ready_cb frees memory that the loop's a_next
  // pointer (to Opa) still references.
  lv_obj_del_async((lv_obj_t*)lv_anim_get_user_data(a));
}

// Sedentary nudge: after this much LVGL inactivity the blob goes droopy.
// Mood knock is applied once per episode; clears on next detected activity.
static const uint32_t SEDENTARY_IDLE_MS   = 60UL * 60UL * 1000UL; // 1 hour
static const uint32_t SEDENTARY_CHECK_MS  = 60000UL;               // poll interval
static const int      SEDENTARY_MOOD_KNOCK = 5;                    // one-time mood cost

// Integer cos/sin lookup for 12 equally-spaced burst directions (×100).
// Angles: 0, 30, 60, … 330 degrees.
static const int BURST_COS[12] = { 100,  87,  50,   0, -50, -87, -100, -87, -50,   0,  50,  87 };
static const int BURST_SIN[12] = {   0,  50,  87, 100,  87,  50,    0, -50, -87, -100, -87, -50 };
static const int BURST_N       = 12;
static const int BURST_R       = 8;     // particle radius (px)
static const int BURST_TRAVEL  = 140;   // outward distance (px)
static const int BURST_MS      = 750;   // total animation duration
static const int BURST_FADE_DL = 200;   // fade starts after this many ms

static void roamArrivalCB(lv_anim_t* a) {
  PetUI* self = (PetUI*)lv_anim_get_user_data(a);
  self->startIdleWobble();
}

static void placeEye(lv_obj_t* eye, lv_coord_t cx, lv_coord_t cy, int w, int h) {
  lv_obj_set_size(eye, w, h);
  lv_obj_align(eye, LV_ALIGN_CENTER, cx, cy);
}

/* ------------------------------------------------------------------ */

void PetUI::init(Pet* petPtr, HabitTracker* trackerPtr) {
  pet      = petPtr;
  tracker  = trackerPtr;
  roamTimer = nullptr;
  blinkTimer = nullptr;
  sedentaryActive  = false;
  sedentaryTimer   = nullptr;
  batteryPct       = -1;
  batteryWarnShown = false;
  blobBaseX = blobBaseY = -1;
  eyeDriftX = eyeDriftY = 0;
  workoutReps = 0; workoutRunning = false;
  workoutDifficulty = DIFF_MEDIUM;
  workoutElapsedMs = 0; workoutClockTimer = nullptr;
  pomState = POM_IDLE; pomLastTapMs = 0; pomRunning = false;
  pomBlocks = 0; pomStartMs = 0; pomElapsedMs = 0; pomClockTimer = nullptr;
  questCount = 0; goalCount = 0;
  lastGestureMs = 0; lastPatMs = 0; syncRequested = false;
  syncOverlay = nullptr; syncLabel = nullptr; syncSpinner = nullptr;
  syncTimeoutTimer = nullptr;
  srand((unsigned)lv_tick_get() ^ 0xBEEF);
  buildPetScreen();
  buildHabitScreen();
  buildQuestScreen();
  buildGoalScreen();
  buildAppsScreen();
  buildWorkoutScreen();
  buildPomodoroScreen();
  sedentaryTimer = lv_timer_create(sedentaryCheckCB, SEDENTARY_CHECK_MS, this);

  lv_scr_load(petScreen);
  refreshPetScreen();
}

void PetUI::buildPetScreen() {
  petScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(petScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(petScreen, LV_OBJ_FLAG_SCROLLABLE);

  blobShape = lv_obj_create(petScreen);
  lv_obj_set_size(blobShape, 100, 100);
  lv_obj_set_pos(blobShape, 0, 0);  // roaming sets real position via lv_obj_set_pos in refreshPetScreen
  lv_obj_set_style_radius(blobShape, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(blobShape, 0, 0);
  lv_obj_set_style_pad_all(blobShape, 0, 0);
  lv_obj_clear_flag(blobShape, LV_OBJ_FLAG_SCROLLABLE);

  eyeLeft  = lv_obj_create(blobShape);
  eyeRight = lv_obj_create(blobShape);
  makeCircle(eyeLeft,  lv_color_hex(0x0A0A0A));
  makeCircle(eyeRight, lv_color_hex(0x0A0A0A));

  stageLabel = lv_label_create(petScreen);
  lv_obj_align(stageLabel, LV_ALIGN_CENTER, 0, 60);
  lv_obj_set_style_text_color(stageLabel, lv_color_white(), 0);

  xpLabel = lv_label_create(petScreen);
  lv_obj_align(xpLabel, LV_ALIGN_CENTER, 0, 90);
  lv_obj_set_style_text_color(xpLabel, lv_color_hex(0x4A6080), 0);

  moodBar = lv_bar_create(petScreen);
  lv_obj_set_size(moodBar, 80, 6);
  lv_obj_set_pos(moodBar, 251, 64);
  lv_bar_set_range(moodBar, 0, 100);
  lv_obj_set_style_bg_color(moodBar, lv_color_hex(0x0E0E1C), 0);
  lv_obj_set_style_bg_opa(moodBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(moodBar, lv_color_hex(0x3ED9AA), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(moodBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(moodBar, 5, 0);
  lv_obj_set_style_radius(moodBar, 5, LV_PART_INDICATOR);

  // --- hunger column (left of top-center; the round glass clips the actual
  //     corners, so both status columns flank the centerline at x=130/246) ---
  lv_obj_t* hungerCaption = lv_label_create(petScreen);
  lv_label_set_text(hungerCaption, "hunger");
  lv_obj_set_pos(hungerCaption, 130, 30);
  lv_obj_set_width(hungerCaption, 90);
  lv_obj_set_style_text_align(hungerCaption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(hungerCaption, lv_color_hex(0x2A4A35), 0);

  hungerLabel = lv_label_create(petScreen);
  lv_obj_set_pos(hungerLabel, 130, 46);
  lv_obj_set_width(hungerLabel, 90);
  lv_obj_set_style_text_align(hungerLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(hungerLabel, lv_color_hex(0x3EE8A0), 0);

  hungerBar = lv_bar_create(petScreen);
  lv_obj_set_size(hungerBar, 80, 6);
  lv_obj_set_pos(hungerBar, 135, 64);
  lv_bar_set_range(hungerBar, 0, 100);
  lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0x0E0E1C), 0);
  lv_obj_set_style_bg_opa(hungerBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0x3EE8A0), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(hungerBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(hungerBar, 5, 0);
  lv_obj_set_style_radius(hungerBar, 5, LV_PART_INDICATOR);

  // --- mood column (right of top-center, mirror of hunger at x=246..336) ---
  lv_obj_t* moodCaption = lv_label_create(petScreen);
  lv_label_set_text(moodCaption, "mood");
  lv_obj_set_width(moodCaption, 90);
  lv_obj_set_pos(moodCaption, 246, 30);
  lv_obj_set_style_text_align(moodCaption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(moodCaption, lv_color_hex(0x2A4A35), 0);

  moodLabel = lv_label_create(petScreen);
  lv_obj_set_width(moodLabel, 90);
  lv_obj_set_pos(moodLabel, 246, 46);
  lv_obj_set_style_text_align(moodLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(moodLabel, lv_color_hex(0x3EE8A0), 0);

  lv_obj_add_event_cb(petScreen, petGestureCB, LV_EVENT_GESTURE, this);
  lv_obj_add_flag(petScreen, LV_OBJ_FLAG_CLICKABLE);
  // Press & hold anywhere on the pet screen = manual sync.
  lv_obj_add_event_cb(petScreen, petLongPressCB, LV_EVENT_LONG_PRESSED, this);
  // A hold that starts on the blob should sync too (events bubble up to the
  // screen); a quick pat released before the long-press threshold is the
  // love reaction instead.
  lv_obj_add_flag(blobShape, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(blobShape, blobPatCB, LV_EVENT_SHORT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(petScreen);
  lv_label_set_text(hint,
      LV_SYMBOL_LEFT " habits    " LV_SYMBOL_RIGHT " quests\n"
      LV_SYMBOL_UP " workout   " LV_SYMBOL_DOWN " focus");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  // Helper text stays small: at the 20 pt default the two-line hint would
  // collide with the battery label and crowd the bottom of the round glass.
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  batteryLabel = lv_label_create(petScreen);
  lv_obj_align(batteryLabel, LV_ALIGN_BOTTOM_MID, 0, -82);
  lv_obj_set_style_text_color(batteryLabel, lv_color_hex(0x3EE8A0), 0);
  lv_label_set_text(batteryLabel, "");
  lv_obj_add_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN);

  exprTimer  = lv_timer_create(exprTimerCB,  3000, this);
  blinkTimer = lv_timer_create(blinkTimerCB, 4500, this);
}

void PetUI::buildHabitScreen() {
  habitScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(habitScreen, lv_color_hex(0x000000), 0);

  lv_obj_t* title = lv_label_create(habitScreen);
  lv_label_set_text(title, "Today's Habits");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_color(title, lv_color_hex(0x9090CC), 0);

  habitList = lv_list_create(habitScreen);
  // Max round-safe footprint: 350×336 with 30px corner radius keeps the
  // rounded corners ~3px inside the glass (√(145²+138²)+30 ≈ 230 < 233).
  lv_obj_set_size(habitList, 350, 336);
  lv_obj_align(habitList, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(habitList, 30, 0);
  lv_obj_set_style_clip_corner(habitList, true, 0);  // keep rows inside the rounded border
  lv_obj_set_style_bg_color(habitList, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(habitList, lv_color_hex(0x1A1A2E), 0);
  lv_obj_set_style_border_width(habitList, 1, 0);
  lv_obj_set_style_pad_row(habitList, 6, 0);
  lv_obj_set_style_pad_all(habitList, 8, 0);
  // Vertical-only scrolling so a horizontal swipe is never consumed as a
  // scroll and always reaches the screen's gesture handler below.
  lv_obj_set_scroll_dir(habitList, LV_DIR_VER);

  lv_obj_t* exitHint = lv_label_create(habitScreen);
  lv_label_set_text(exitHint, "swipe " LV_SYMBOL_RIGHT " for pet");
  lv_obj_align(exitHint, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_set_style_text_color(exitHint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(exitHint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(habitScreen, habitGestureCB, LV_EVENT_GESTURE, this);
}

/* ---- quest screen ------------------------------------------------ */

void PetUI::buildQuestScreen() {
  questScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(questScreen, lv_color_hex(0x000000), 0);

  lv_obj_t* title = lv_label_create(questScreen);
  lv_label_set_text(title, "Quests");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_color(title, lv_color_hex(0xCCA050), 0);

  questList = lv_list_create(questScreen);
  lv_obj_set_size(questList, 350, 336);  // same max round-safe footprint as the habit list
  lv_obj_align(questList, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(questList, 30, 0);
  lv_obj_set_style_clip_corner(questList, true, 0);
  lv_obj_set_style_bg_color(questList, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(questList, lv_color_hex(0x2E2210), 0);
  lv_obj_set_style_border_width(questList, 1, 0);
  lv_obj_set_style_pad_row(questList, 6, 0);
  lv_obj_set_style_pad_all(questList, 8, 0);
  lv_obj_set_scroll_dir(questList, LV_DIR_VER);  // keep horizontal swipes for the exit gesture

  lv_obj_t* exitHint = lv_label_create(questScreen);
  lv_label_set_text(exitHint, LV_SYMBOL_LEFT " pet    " LV_SYMBOL_RIGHT " goals");
  lv_obj_align(exitHint, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_set_style_text_color(exitHint, lv_color_hex(0x44341A), 0);
  lv_obj_set_style_text_font(exitHint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(questScreen, questGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::refreshQuestScreen() {
  lv_obj_clean(questList);
  if (questCount == 0) {
    lv_obj_t* empty = lv_label_create(questList);
    lv_label_set_text(empty, "no active quests\n\nadd some on the dashboard");
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x554422), 0);
    lv_obj_center(empty);
    return;
  }
  for (int i = 0; i < questCount; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  +%dxp", quests[i].name, quests[i].xp);
    lv_obj_t* btn = lv_list_add_btn(questList, LV_SYMBOL_GPS, buf);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x171205), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xCCA050), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);  // read-only: completed on the dashboard
  }
}

void PetUI::showQuestScreen() {
  refreshQuestScreen();
  // Opened by swipe-right from the pet screen, so slide in from the left.
  lv_scr_load_anim(questScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

void PetUI::setQuests(const QuestInfo* q, int count) {
  if (count > MAX_QUESTS) count = MAX_QUESTS;
  if (count < 0) count = 0;
  questCount = count;
  for (int i = 0; i < count; i++) quests[i] = q[i];
  refreshQuestScreen();
}

void PetUI::questGestureCB(lv_event_t* e) {
  // Horizontal carousel: pet < quests > goals. Left walks back to the pet,
  // right continues deeper to the goal screen.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  switch (lv_indev_get_gesture_dir(lv_indev_get_act())) {
    case LV_DIR_LEFT:
      lv_scr_load_anim(self->petScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
      self->refreshPetScreen();
      break;
    case LV_DIR_RIGHT:
      self->showGoalScreen();
      break;
    default:
      break;
  }
}

/* ---- goal screen -------------------------------------------------- */

void PetUI::buildGoalScreen() {
  goalScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(goalScreen, lv_color_hex(0x000000), 0);

  lv_obj_t* title = lv_label_create(goalScreen);
  lv_label_set_text(title, "Goals");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_color(title, lv_color_hex(0x50C8A8), 0);

  goalList = lv_list_create(goalScreen);
  lv_obj_set_size(goalList, 350, 336);  // same max round-safe footprint as the habit list
  lv_obj_align(goalList, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(goalList, 30, 0);
  lv_obj_set_style_clip_corner(goalList, true, 0);
  lv_obj_set_style_bg_color(goalList, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(goalList, lv_color_hex(0x102E24), 0);
  lv_obj_set_style_border_width(goalList, 1, 0);
  lv_obj_set_style_pad_row(goalList, 6, 0);
  lv_obj_set_style_pad_all(goalList, 8, 0);
  lv_obj_set_scroll_dir(goalList, LV_DIR_VER);  // keep horizontal swipes for the exit gesture

  lv_obj_t* exitHint = lv_label_create(goalScreen);
  lv_label_set_text(exitHint, "swipe " LV_SYMBOL_LEFT " for quests");
  lv_obj_align(exitHint, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_set_style_text_color(exitHint, lv_color_hex(0x1A4436), 0);
  lv_obj_set_style_text_font(exitHint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(goalScreen, goalGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::refreshGoalScreen() {
  lv_obj_clean(goalList);
  if (goalCount == 0) {
    lv_obj_t* empty = lv_label_create(goalList);
    lv_label_set_text(empty, "no active goals\n\nadd some on the dashboard");
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x225544), 0);
    lv_obj_center(empty);
    return;
  }
  for (int i = 0; i < goalCount; i++) {
    char buf[80];
    if (goals[i].period[0] != '\0')
      snprintf(buf, sizeof(buf), "%s  +%dxp (%s)", goals[i].name, goals[i].xp, goals[i].period);
    else
      snprintf(buf, sizeof(buf), "%s  +%dxp", goals[i].name, goals[i].xp);
    lv_obj_t* btn = lv_list_add_btn(goalList, LV_SYMBOL_LOOP, buf);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x051712), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x50C8A8), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);  // read-only: completed on the dashboard
  }
}

void PetUI::showGoalScreen() {
  refreshGoalScreen();
  // Opened by swipe-right from the quest screen, so slide in from the left.
  lv_scr_load_anim(goalScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

void PetUI::setGoals(const GoalInfo* g, int count) {
  if (count > MAX_GOALS) count = MAX_GOALS;
  if (count < 0) count = 0;
  goalCount = count;
  for (int i = 0; i < count; i++) goals[i] = g[i];
  refreshGoalScreen();
}

void PetUI::goalGestureCB(lv_event_t* e) {
  // Exit is the opposite of the opening swipe (right to open, left to close).
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_LEFT) return;
  lv_scr_load_anim(self->questScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

/* ---- manual sync (press & hold) + overlay ------------------------ */

void PetUI::petLongPressCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->syncRequested = true;
  self->syncStarted();
}

/* ---- blob pat reaction -------------------------------------------- */

void PetUI::blobPatCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  uint32_t now = lv_tick_get();
  // A swipe that starts on the blob also emits a short click on release.
  if (now - self->lastGestureMs < 600) return;
  // Debounce: caps hearts/timers/anim churn when the blob gets mashed.
  if (now - self->lastPatMs < 250) return;
  self->lastPatMs = now;
  self->blobLoveReaction();
}

void PetUI::blobLoveReaction() {
  // Happy squint (^ ^): wide, nearly-flat eyes, back to normal after a moment.
  if (eyeBaseW > 0) {
    int ew = eyeBaseW * 3 / 2;
    int eh = LV_MAX(3, eyeBaseW / 5);
    placeEye(eyeLeft,  -eyeOffX + eyeDriftX, eyeOffY + eyeDriftY, ew, eh);
    placeEye(eyeRight,  eyeOffX + eyeDriftX, eyeOffY + eyeDriftY, ew, eh);
    lv_timer_t* rv = lv_timer_create(revertExprCB, 900, this);
    lv_timer_set_repeat_count(rv, 1);
  }

  // Excited puff: a quick width/height grow-and-shrink — the same anim
  // technique the landing squash uses, which is proven safe on-device.
  // (A style transform_zoom crashed the real renderer here; LVGL v8 only
  // reliably supports transforms on images, not plain objects.)
  // X/Y are deliberately untouched: they're owned by the roam/wobble anims.
  //
  // Base size MUST come from the stage, not lv_obj_get_width(): a pat that
  // lands mid-puff would read the inflated width and each spam-tap would
  // ratchet the blob (and its glow shadow) bigger until the shadow draw
  // buffer exhausts LVGL's memory pool and crashes the device.
  lv_coord_t sz = stageSize(pet->getStage());
  lv_coord_t puff = sz + sz / 8;
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetW);
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetH);
  lv_obj_set_size(blobShape, sz, sz);  // snap back to base before re-puffing
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetW);
  lv_anim_set_values(&a, sz, puff);
  lv_anim_set_time(&a, 120);
  lv_anim_set_playback_time(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetH);
  lv_anim_set_values(&a, sz, puff);
  lv_anim_set_time(&a, 120);
  lv_anim_set_playback_time(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  // A little heart drifts up from the blob and fades out.
  lv_obj_t* heart = lv_label_create(petScreen);
  lv_label_set_text(heart, "<3");
  lv_obj_set_style_text_color(heart, lv_color_hex(0xFF6090), 0);
  lv_coord_t hx = lv_obj_get_x(blobShape) + sz / 2 - 12 + (rand() % 25 - 12);
  lv_coord_t hy = lv_obj_get_y(blobShape) - 14;
  lv_obj_set_pos(heart, hx, hy);

  lv_anim_init(&a);
  lv_anim_set_var(&a, heart);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, hy, hy - 60);
  lv_anim_set_time(&a, 900);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, heart);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, 300);
  lv_anim_set_time(&a, 600);
  lv_anim_set_user_data(&a, heart);
  lv_anim_set_ready_cb(&a, xpPopupDeleteCB);  // same del_async pattern as the xp pill
  lv_anim_start(&a);
}

bool PetUI::consumeSyncRequest() {
  bool r = syncRequested;
  syncRequested = false;
  return r;
}

void PetUI::syncStarted() {
  if (syncOverlay) return;

  syncOverlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(syncOverlay, 200, 56);
  lv_obj_align(syncOverlay, LV_ALIGN_CENTER, 0, -140);
  lv_obj_set_style_bg_color(syncOverlay, lv_color_hex(0x060D1A), 0);
  lv_obj_set_style_bg_opa(syncOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(syncOverlay, 28, 0);
  lv_obj_set_style_border_color(syncOverlay, lv_color_hex(0x2050A0), 0);
  lv_obj_set_style_border_width(syncOverlay, 1, 0);
  lv_obj_set_style_pad_all(syncOverlay, 0, 0);
  lv_obj_clear_flag(syncOverlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  syncSpinner = lv_spinner_create(syncOverlay, 1000, 60);
  lv_obj_set_size(syncSpinner, 32, 32);
  lv_obj_align(syncSpinner, LV_ALIGN_LEFT_MID, 14, 0);
  lv_obj_set_style_arc_width(syncSpinner, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(syncSpinner, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(syncSpinner, lv_color_hex(0x122040), LV_PART_MAIN);
  lv_obj_set_style_arc_color(syncSpinner, lv_color_hex(0x4090FF), LV_PART_INDICATOR);

  syncLabel = lv_label_create(syncOverlay);
  lv_label_set_text(syncLabel, "syncing...");
  lv_obj_set_style_text_color(syncLabel, lv_color_hex(0x4090FF), 0);
  lv_obj_align(syncLabel, LV_ALIGN_LEFT_MID, 58, 0);

  // Safety net: if loop() never reports back (e.g. running in the sim with
  // nothing consuming the request), dismiss as failed after 10 s.
  syncTimeoutTimer = lv_timer_create(syncTimeoutCB, 10000, this);
  lv_timer_set_repeat_count(syncTimeoutTimer, 1);
}

void PetUI::syncFinished(bool ok) {
  if (!syncOverlay) return;
  if (syncTimeoutTimer) { lv_timer_del(syncTimeoutTimer); syncTimeoutTimer = nullptr; }

  lv_obj_del(syncSpinner);
  syncSpinner = nullptr;

  lv_color_t col = ok ? lv_color_hex(0x3EE8A0) : lv_color_hex(0xFF4040);
  lv_label_set_text(syncLabel, ok ? LV_SYMBOL_OK "  synced" : LV_SYMBOL_CLOSE "  offline");
  lv_obj_set_style_text_color(syncLabel, col, 0);
  lv_obj_align(syncLabel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_border_color(syncOverlay, col, 0);

  // Hold the result for a moment, then fade out and delete.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, syncOverlay);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, 1200);
  lv_anim_set_time(&a, 600);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, syncOverlayDeleteCB);
  lv_anim_set_user_data(&a, this);
  lv_anim_start(&a);
}

void PetUI::syncTimeoutCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  self->syncTimeoutTimer = nullptr;  // one-shot timer deletes itself
  self->syncRequested = false;
  self->syncFinished(false);
}

void PetUI::syncOverlayDeleteCB(lv_anim_t* a) {
  PetUI* self = (PetUI*)lv_anim_get_user_data(a);
  if (self->syncOverlay) {
    lv_obj_del(self->syncOverlay);
    self->syncOverlay = nullptr;
    self->syncLabel   = nullptr;
  }
}

/* ---- pet screen refresh ------------------------------------------ */

void PetUI::refreshPetScreen() {
  int stage = pet->getStage();
  int sz    = stageSize(stage);

  // On first call, place blob at screen center and seed blobBaseX/Y.
  // On subsequent calls keep the current roam position.
  if (blobBaseX < 0) {
    lv_coord_t pW = lv_obj_get_width(petScreen);
    lv_coord_t pH = lv_obj_get_height(petScreen);
    blobBaseX = (pW - sz) / 2;
    blobBaseY = (pH - sz) / 2 - 40;
    lv_obj_set_pos(blobShape, blobBaseX, blobBaseY);
  }

  lv_obj_set_size(blobShape, sz, sz);
  lv_obj_set_style_bg_color(blobShape, lv_color_hex(0xFFFFFF), 0);

  // Glow: solid white ring (spread), tiny soft blur edge (width).
  // mood=0 → 25% ring width, mood=100 → 100%. Width stays small so the ring is white not grey.
  int mood = pet->getMood();
  int glowFactor = 25 + 75 * mood / 100;
  lv_obj_set_style_shadow_color(blobShape, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_shadow_opa(blobShape, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_spread(blobShape, GLOW_SPREAD[stage] * glowFactor / 100, 0);
  lv_obj_set_style_shadow_ofs_x(blobShape, 0, 0);
  lv_obj_set_style_shadow_ofs_y(blobShape, 0, 0);
  lv_obj_set_style_shadow_width(blobShape, GLOW_BASE_W[stage], 0);

  // Eye layout (relative to current blob size)
  eyeOffX  = sz * 3 / 10;
  eyeOffY  = -(sz / 10);
  eyeBaseW = sz / 5;

  applyMoodExpression();
  startBlobAnimations();

  // Label: stage name + level
  int level = pet->getLevel();
  lv_label_set_text_fmt(stageLabel, "%s  LV.%d", pet->getStageName(), level);

  // XP progress within current level
  int xpIn   = pet->xpIntoCurrentLevel();
  int xpNeed = xpIn + pet->xpToNextLevel();
  lv_label_set_text_fmt(xpLabel, "%d / %d xp", xpIn, xpNeed);

  // Expression timer fires faster at higher stages (more energy)
  lv_timer_set_period(exprTimer, EXPR_PERIOD_MS[stage]);

  lv_bar_set_value(moodBar, pet->getMood(), LV_ANIM_ON);

  // Hunger corner (top-left)
  if (!pet->isAlive()) {
    lv_label_set_text(hungerLabel, LV_SYMBOL_CLOSE " DEAD");
    lv_obj_set_style_text_color(hungerLabel, lv_color_hex(0xFF3030), 0);
    lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0xFF3030), LV_PART_INDICATOR);
    lv_bar_set_value(hungerBar, 0, LV_ANIM_ON);
  } else {
    int h = pet->getHunger();
    lv_bar_set_value(hungerBar, h, LV_ANIM_ON);
    if (h >= 70) {
      lv_label_set_text_fmt(hungerLabel, LV_SYMBOL_OK " %d%%", h);
      lv_obj_set_style_text_color(hungerLabel, lv_color_hex(0x3EE8A0), 0);
      lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0x3EE8A0), LV_PART_INDICATOR);
    } else if (h >= 40) {
      lv_label_set_text_fmt(hungerLabel, LV_SYMBOL_WARNING " %d%%", h);
      lv_obj_set_style_text_color(hungerLabel, lv_color_hex(0xD4A030), 0);
      lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0xD4A030), LV_PART_INDICATOR);
    } else if (h >= 20) {
      lv_label_set_text_fmt(hungerLabel, LV_SYMBOL_WARNING " %d%%", h);
      lv_obj_set_style_text_color(hungerLabel, lv_color_hex(0xFF8040), 0);
      lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0xFF8040), LV_PART_INDICATOR);
    } else {
      lv_label_set_text(hungerLabel, LV_SYMBOL_WARNING " STARV");
      lv_obj_set_style_text_color(hungerLabel, lv_color_hex(0xFF4040), 0);
      lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0xFF4040), LV_PART_INDICATOR);
    }
  }

  // Mood corner (top-right)
  {
    int m = pet->getMood();
    if (m >= 70) {
      lv_label_set_text_fmt(moodLabel, "%d%% " LV_SYMBOL_OK, m);
      lv_obj_set_style_text_color(moodLabel, lv_color_hex(0x3EE8A0), 0);
    } else if (m >= 40) {
      lv_label_set_text_fmt(moodLabel, "%d%% " LV_SYMBOL_MINUS, m);
      lv_obj_set_style_text_color(moodLabel, lv_color_hex(0xD4A030), 0);
    } else {
      lv_label_set_text_fmt(moodLabel, "%d%% " LV_SYMBOL_WARNING, m);
      lv_obj_set_style_text_color(moodLabel, lv_color_hex(0xFF4040), 0);
    }
    lv_obj_set_pos(moodLabel, 246, 46);
  }
}

/* ---- animations -------------------------------------------------- */

void PetUI::startBlobAnimations() {
  int stage = pet->getStage();

  // Cancel everything and kill the old roam timer
  lv_anim_del(blobShape, NULL);
  if (roamTimer) { lv_timer_del(roamTimer); roamTimer = nullptr; }

  // Pulsing glow — spread oscillates (solid white ring breathes)
  {
    int mood = pet->getMood();
    int gf     = 25 + 75 * mood / 100;
    int gBase  = GLOW_SPREAD[stage] * gf / 100;
    int gPulse = GLOW_PULSE[stage]  * gf / 100;
    int gLo = LV_MAX(0, gBase - gPulse);
    int gHi = gBase + gPulse;
    if (gHi > 0) {
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, blobShape);
      lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetGlow);
      lv_anim_set_values(&a, gLo, gHi);
      lv_anim_set_time(&a, GLOW_PULSE_MS[stage]);
      lv_anim_set_playback_time(&a, GLOW_PULSE_MS[stage]);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
      lv_anim_start(&a);
    }
  }

  // Kick off roaming — first move is immediate, then on timer
  // Period scales with mood: mood=100 → 1× base, mood=0 → 5× base
  roamToRandom();
  int mood0 = pet->getMood();
  uint32_t initPeriod = (uint32_t)(ROAM_PERIOD_MS[stage] * (25 + (100 - mood0)) / 25);
  roamTimer = lv_timer_create(roamTimerCB, initPeriod, this);
}

void PetUI::roamToRandom() {
  int stage = pet->getStage();
  int sz    = stageSize(stage);
  lv_coord_t pW = lv_obj_get_width(petScreen);
  lv_coord_t pH = lv_obj_get_height(petScreen);

  // Cancel squash/stretch and reset to normal size before starting new roam
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetX);
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetW);
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetH);
  lv_obj_set_size(blobShape, sz, sz);
  if (blobBaseX >= 0) lv_obj_set_pos(blobShape, blobBaseX, blobBaseY);

  // Keep blob + glow fully within screen.
  int glowExt = GLOW_BASE_W[stage] + GLOW_PULSE[stage] + GLOW_SPREAD[stage];
  int margin  = 10 + glowExt;
  int rangeX  = pW - sz - margin * 2;
  int rangeY  = pH - sz - margin * 2;

  int targetX = rangeX > 0 ? margin + rand() % rangeX : (pW - sz) / 2;
  int targetY = rangeY > 0 ? margin + rand() % rangeY : (pH - sz) / 2;

  int curX = blobBaseX;
  int curY = blobBaseY;
  blobBaseX = targetX;
  blobBaseY = targetY;

  // Eye drift: shift eyes slightly toward the direction of movement
  int dx = targetX - curX;
  int dy = targetY - curY;
  int absDx = dx < 0 ? -dx : dx;
  int absDy = dy < 0 ? -dy : dy;
  int dist  = absDx + absDy;
  if (dist > 4) {
    eyeDriftX = (lv_coord_t)(dx * 7 / dist);
    eyeDriftY = (lv_coord_t)(dy * 7 / dist);
  } else {
    eyeDriftX = eyeDriftY = 0;
  }
  applyMoodExpression();

  // Directional stretch: blob elongates in the direction of travel
  int stretchW, stretchH;
  if (absDx >= absDy) {
    stretchW = sz * 12 / 10;  // wider for horizontal movement
    stretchH = sz *  8 / 10;
  } else {
    stretchW = sz *  8 / 10;  // taller for vertical movement
    stretchH = sz * 12 / 10;
  }

  int travelMs = ROAM_TRAVEL_MS[stage];
  lv_anim_t a;

  // Stretch W during flight
  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetW);
  lv_anim_set_values(&a, sz, stretchW);
  lv_anim_set_time(&a, travelMs * 3 / 5);
  lv_anim_set_playback_time(&a, travelMs * 2 / 5);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);

  // Stretch H during flight
  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetH);
  lv_anim_set_values(&a, sz, stretchH);
  lv_anim_set_time(&a, travelMs * 3 / 5);
  lv_anim_set_playback_time(&a, travelMs * 2 / 5);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);

  // Glide X
  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetX);
  lv_anim_set_values(&a, curX, targetX);
  lv_anim_set_time(&a, travelMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);

  // Glide Y — arrival callback starts the idle wobble
  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, curY, targetY);
  lv_anim_set_time(&a, travelMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_ready_cb(&a, roamArrivalCB);
  lv_anim_set_user_data(&a, this);
  lv_anim_start(&a);
}

void PetUI::startIdleWobble() {
  int stage = pet->getStage();
  int sz    = stageSize(stage);

  // Ensure size is reset to normal in case stretch anim didn't finish cleanly
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetW);
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetH);
  lv_obj_set_size(blobShape, sz, sz);
  lv_obj_set_pos(blobShape, blobBaseX, blobBaseY);

  // Center eyes (drift reset)
  eyeDriftX = eyeDriftY = 0;
  applyMoodExpression();

  // Squash on landing: blob flattens wide + short, then springs back
  int sqW    = sz * 5 / 4;          // 125% wide
  int sqH    = sz * 3 / 4;          // 75% tall
  int xShift = (sqW - sz) / 2;      // shift left to keep center fixed

  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetW);
  lv_anim_set_values(&a, sz, sqW);
  lv_anim_set_time(&a, 130);
  lv_anim_set_playback_time(&a, 210);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetH);
  lv_anim_set_values(&a, sz, sqH);
  lv_anim_set_time(&a, 130);
  lv_anim_set_playback_time(&a, 210);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  // X compensation: shift left while wider, restore — keeps visual center fixed
  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetX);
  lv_anim_set_values(&a, blobBaseX, blobBaseX - xShift);
  lv_anim_set_time(&a, 130);
  lv_anim_set_playback_time(&a, 210);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  // Idle wobble Y — delayed until squash finishes (130+210 = 340ms)
  int curY = blobBaseY;
  int amp  = BOUNCE_AMP_PX[stage];
  int ms   = BOUNCE_MS[stage];

  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, curY, curY - amp);
  lv_anim_set_time(&a, ms);
  lv_anim_set_delay(&a, 340);
  lv_anim_set_playback_time(&a, ms);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

void PetUI::roamTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  // Recompute period so mood changes take effect each interval
  int stage = self->pet->getStage();
  int mood  = self->pet->getMood();
  uint32_t period = (uint32_t)(ROAM_PERIOD_MS[stage] * (25 + (100 - mood)) / 25);
  lv_timer_set_period(t, period);
  self->roamToRandom();
}

void PetUI::blinkTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  int bw = self->eyeBaseW;
  if (bw <= 0) return;
  // Snap eyes shut
  placeEye(self->eyeLeft,  -self->eyeOffX + self->eyeDriftX, self->eyeOffY + self->eyeDriftY, bw, 1);
  placeEye(self->eyeRight,  self->eyeOffX + self->eyeDriftX, self->eyeOffY + self->eyeDriftY, bw, 1);
  // Reopen after 110ms using the existing revertExprCB (calls applyMoodExpression)
  lv_timer_t* rv = lv_timer_create(revertExprCB, 110, self);
  lv_timer_set_repeat_count(rv, 1);
  // Randomize next blink: 3–6 seconds
  lv_timer_set_period(t, 3000 + (lv_tick_get() % 3000));
}

/* ---- expression system ------------------------------------------- */

void PetUI::applyMoodExpression() {
  int mood = pet->getMood();
  int ew, eh;
  lv_coord_t extraY = 0;
  if (sedentaryActive) {
    // Heavy-lidded droopy: wider + very flat eyes, shifted down
    ew     = eyeBaseW * 3 / 2;
    eh     = LV_MAX(2, eyeBaseW / 5);
    extraY = LV_MAX(1, eyeBaseW / 4);
  } else if (mood >= 70) {
    ew = eyeBaseW * 6 / 5; eh = ew;
  } else if (mood >= 30) {
    ew = eyeBaseW; eh = ew;
  } else {
    ew = eyeBaseW * 5 / 4;
    eh = LV_MAX(3, eyeBaseW / 4);
  }
  // Shift eyes in the direction the blob is moving (cleared on landing)
  placeEye(eyeLeft,  -eyeOffX + eyeDriftX, eyeOffY + eyeDriftY + extraY, ew, eh);
  placeEye(eyeRight,  eyeOffX + eyeDriftX, eyeOffY + eyeDriftY + extraY, ew, eh);
}

void PetUI::exprTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  static uint8_t tick = 0;
  tick++;

  int bw = self->eyeBaseW;
  int ew, eh;

  if (tick % 5 == 0) {
    // Surprised: large round
    ew = bw * 3 / 2; eh = ew;
  } else if (tick % 5 == 2) {
    // Squint: wide, nearly flat
    ew = bw * 5 / 4; eh = LV_MAX(3, bw / 5);
  } else {
    self->applyMoodExpression();
    return;
  }

  placeEye(self->eyeLeft,  -self->eyeOffX, self->eyeOffY, ew, eh);
  placeEye(self->eyeRight,  self->eyeOffX, self->eyeOffY, ew, eh);

  lv_timer_t* rv = lv_timer_create(revertExprCB, 700, self);
  lv_timer_set_repeat_count(rv, 1);
}

void PetUI::revertExprCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  self->applyMoodExpression();
}

/* ---- screen transitions ------------------------------------------ */

void PetUI::showPetScreen() {
  lv_scr_load_anim(petScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
  refreshPetScreen();
}

void PetUI::showHabitScreen() {
  refreshHabitScreen();
  lv_scr_load_anim(habitScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

/* ---- habit screen ------------------------------------------------ */

void PetUI::refreshHabitScreen() {
  lv_obj_clean(habitList);
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (!h->active) continue;

    // Show what the NEXT tap is worth (streak bonus included) — for done
    // habits, what today's completion earned. Seeing "+15xp" become "+22xp"
    // as the streak climbs is the point of the multiplier.
    int shownXp = h->doneToday ? habitAwardXP(h->xpValue, h->streak)
                               : habitAwardXP(h->xpValue, h->streak + 1);
    char buf[64];
    int pct = streakBonusPercent(h->doneToday ? h->streak : h->streak + 1);
    if (pct > 0)
      snprintf(buf, sizeof(buf), "%s  +%dxp  streak %d (+%d%%)", h->name, shownXp, h->streak, pct);
    else
      snprintf(buf, sizeof(buf), "%s  +%dxp  streak %d", h->name, shownXp, h->streak);

    lv_color_t bgColor   = h->doneToday ? lv_color_hex(0x071C14) : lv_color_hex(0x0D0D1F);
    lv_color_t textColor = h->doneToday ? lv_color_hex(0x3EE8A0) : lv_color_hex(0x8A9ACC);

    lv_obj_t* btn = lv_list_add_btn(habitList,
                      h->doneToday ? LV_SYMBOL_OK : LV_SYMBOL_BULLET, buf);
    lv_obj_set_style_bg_color(btn, bgColor, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);

    uint32_t child_cnt = lv_obj_get_child_cnt(btn);
    for (uint32_t ci = 0; ci < child_cnt; ci++) {
      lv_obj_set_style_text_color(lv_obj_get_child(btn, ci), textColor, 0);
    }

    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, habitButtonEventCB, LV_EVENT_CLICKED, this);
  }
}

// Perfect-day bonus: finishing the LAST remaining habit of the day pays a
// visible completion bonus on top — a clear daily goal with a payoff spike
// (the "one more to close it out" pull). Deterministic from the habit list
// so the undo path can revert it exactly.
static int perfectDayBonus(HabitTracker* tracker) {
  int active = 0;
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (h->active) active++;
  }
  return active > 0 ? 10 + 5 * active : 0;
}

static bool allHabitsDone(HabitTracker* tracker) {
  bool any = false;
  for (int i = 0; i < MAX_HABITS; i++) {
    Habit* h = tracker->get(i);
    if (!h->active) continue;
    if (!h->doneToday) return false;
    any = true;
  }
  return any;
}

void PetUI::habitButtonEventCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  // A swipe released over a list row also emits CLICKED on that row — the
  // exit gesture must not toggle a habit (same filter as blobPatCB).
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  lv_obj_t* btn = lv_event_get_target(e);
  int index = (int)(intptr_t)lv_obj_get_user_data(btn);
  Habit* h = self->tracker->get(index);
  if (!h || !h->active) return;

  // Capture coords before refreshHabitScreen() destroys btn
  lv_area_t coords;
  lv_obj_get_coords(btn, &coords);

  if (h->doneToday) {
    // Toggle back to undone: revert streak bump, streak-bonus XP, and (if this
    // breaks a perfect day) the perfect-day bonus — the exact award, mirrored.
    int awarded = habitAwardXP(h->xpValue, h->streak);
    if (allHabitsDone(self->tracker)) awarded += perfectDayBonus(self->tracker);
    if (self->tracker->uncompleteHabit(index)) {
      self->pet->removeXP(awarded);
      self->refreshHabitScreen();
      self->showXpPopup(coords, -awarded);
      if (self->soundCB) self->soundCB(SOUND_HABIT_UNDONE);
    }
  } else if (self->tracker->completeHabit(index)) {
    int stageBefore = self->pet->getStage();
    int levelBefore = self->pet->getLevel();

    int awarded = habitAwardXP(h->xpValue, h->streak);  // streak already bumped
    bool perfect = allHabitsDone(self->tracker);
    if (perfect) awarded += perfectDayBonus(self->tracker);
    self->pet->addXP(awarded);

    bool evolved   = self->pet->getStage() > stageBefore;
    bool leveledUp = self->pet->getLevel() > levelBefore;

    self->refreshHabitScreen();
    self->showXpPopup(coords, awarded);
    if (evolved)         self->showEvolutionBurst();
    else if (leveledUp)  self->showLevelUpPill(self->pet->getLevel());
    if (self->soundCB) self->soundCB(SOUND_HABIT_DONE);
  }
}

void PetUI::showXpPopup(lv_area_t coords, int xpValue) {
  // Pill badge: dark bg + gold border + large text, lives on the top layer
  lv_obj_t* pill = lv_obj_create(lv_layer_top());
  lv_obj_set_size(pill, 110, 40);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x1A1200), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pill, 20, 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(0xE3B34F), 0);
  lv_obj_set_style_border_width(pill, 2, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(pill);
  char buf[16];
  snprintf(buf, sizeof(buf), "%+d xp", xpValue);  // "+12 xp" / "-12 xp"
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl,
      lv_color_hex(xpValue >= 0 ? 0xFFD166 : 0xFF8080), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_center(lbl);

  // Place pill centered on the tapped button
  lv_coord_t cx = (coords.x1 + coords.x2) / 2 - 55;  // 55 = half of 110
  lv_coord_t cy = (coords.y1 + coords.y2) / 2 - 20;  // 20 = half of 40
  lv_obj_set_pos(pill, cx, cy);

  lv_anim_t a;

  // Float up 80 px, ease out, 1.4 s
  lv_anim_init(&a);
  lv_anim_set_var(&a, pill);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, cy, cy - 80);
  lv_anim_set_time(&a, 1400);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, xpPopupDeleteCB);
  lv_anim_set_user_data(&a, pill);
  lv_anim_start(&a);

  // Fade out — stays fully visible for 500 ms, then fades over 900 ms
  lv_anim_init(&a);
  lv_anim_set_var(&a, pill);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, 500);
  lv_anim_set_time(&a, 900);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_start(&a);
}

void PetUI::showEvolutionBurst() {
  lv_color_t stageCol = stageColor(pet->getStage());
  lv_coord_t cx = lv_obj_get_width(petScreen)  / 2;
  lv_coord_t cy = lv_obj_get_height(petScreen) / 2;

  for (int i = 0; i < BURST_N; i++) {
    lv_obj_t* p = lv_obj_create(lv_layer_top());
    lv_obj_set_size(p, BURST_R * 2, BURST_R * 2);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Cycle: stage color, white, gold
    lv_color_t col = (i % 3 == 0) ? lv_color_hex(0xFFFFFF) :
                     (i % 3 == 1) ? stageCol :
                                    lv_color_hex(0xFFD166);
    lv_obj_set_style_bg_color(p, col, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);

    lv_coord_t sx = cx - BURST_R;
    lv_coord_t sy = cy - BURST_R;
    lv_coord_t ex = sx + (lv_coord_t)(BURST_COS[i] * BURST_TRAVEL / 100);
    lv_coord_t ey = sy + (lv_coord_t)(BURST_SIN[i] * BURST_TRAVEL / 100);
    lv_obj_set_pos(p, sx, sy);

    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetX);
    lv_anim_set_values(&a, sx, ex);
    lv_anim_set_time(&a, BURST_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
    lv_anim_set_values(&a, sy, ey);
    lv_anim_set_time(&a, BURST_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    // Fade out — reuse xpPopupDeleteCB (same del_async pattern)
    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, BURST_FADE_DL);
    lv_anim_set_time(&a, BURST_MS - BURST_FADE_DL);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, xpPopupDeleteCB);
    lv_anim_set_user_data(&a, p);
    lv_anim_start(&a);
  }
}

// Level-up (without evolution) gets its own celebration: a gold pill at
// screen center. Evolution keeps the bigger particle burst.
void PetUI::showLevelUpPill(int level) {
  lv_obj_t* pill = lv_obj_create(lv_layer_top());
  lv_obj_set_size(pill, 170, 44);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x1A1200), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pill, 22, 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(0xE3B34F), 0);
  lv_obj_set_style_border_width(pill, 2, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_coord_t px = 233 - 85;
  lv_coord_t py = 150;
  lv_obj_set_pos(pill, px, py);

  lv_obj_t* lbl = lv_label_create(pill);
  lv_label_set_text_fmt(lbl, LV_SYMBOL_UP " LEVEL %d!", level);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFD166), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_center(lbl);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, pill);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, py, py - 70);
  lv_anim_set_time(&a, 1600);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, xpPopupDeleteCB);
  lv_anim_set_user_data(&a, pill);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, pill);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, 700);
  lv_anim_set_time(&a, 900);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_start(&a);
}

void PetUI::petGestureCB(lv_event_t* e) {
  // Pet screen is the navigation hub; taps are ignored so accidental
  // touches while glancing at the pet don't navigate away.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();  // suppress the CLICKED this swipe emits on release
  lv_indev_wait_release(lv_indev_get_act());
  switch (lv_indev_get_gesture_dir(lv_indev_get_act())) {
    case LV_DIR_LEFT:
      self->showHabitScreen();
      break;
    case LV_DIR_RIGHT:
      self->showQuestScreen();
      break;
    case LV_DIR_TOP:
      self->showWorkoutScreen();
      break;
    case LV_DIR_BOTTOM:
      self->pomState = POM_IDLE;  // always start fresh from the pet screen
      self->showPomodoroScreen();
      break;
    default:
      break;
  }
}

void PetUI::habitGestureCB(lv_event_t* e) {
  // Exit is the opposite of the opening swipe (left to open, right to close).
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  // Swallow the rest of this press: without this, releasing the swipe over a
  // list row emits CLICKED on that row and toggles a habit. The lastGestureMs
  // check in habitButtonEventCB stays as a second layer.
  lv_indev_wait_release(lv_indev_get_act());
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_RIGHT) return;
  self->showPetScreen();
}

void PetUI::workoutGestureCB(lv_event_t* e) {
  // Opened with swipe up; swipe down closes (same cleanup as the Cancel button).
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  // Also swallows the release-click that would otherwise land on the rep
  // tap zone and count a phantom rep.
  lv_indev_wait_release(lv_indev_get_act());
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_BOTTOM) return;
  workoutBackBtnCB(e);
}

void PetUI::pomGestureCB(lv_event_t* e) {
  // Opened with swipe down; swipe up closes (same cleanup as the Cancel button).
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_TOP) return;
  pomCancelBtnCB(e);
}

/* ---- apps menu ---------------------------------------------------- */

// Launcher entries. Adding a future app = one row here + one case in
// appsBtnCB below; the buttons lay themselves out around screen center.
struct AppEntry {
  const char* icon;
  const char* name;
  uint32_t    color;
};
static const AppEntry APP_ENTRIES[] = {
  { LV_SYMBOL_CHARGE, "workout", 0x3EE8A0 },
  { LV_SYMBOL_EYE_OPEN, "focus",   0xFF8080 },
};
static const int APP_COUNT = sizeof(APP_ENTRIES) / sizeof(APP_ENTRIES[0]);

void PetUI::buildAppsScreen() {
  appsScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(appsScreen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(appsScreen, 0, 0);
  lv_obj_clear_flag(appsScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(appsScreen);
  lv_label_set_text(title, "APPS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_text_color(title, lv_color_hex(0xA080FF), 0);

  // One big rounded button per app, stacked around screen center.
  const int btnH = 72, gap = 20, pitch = btnH + gap;
  for (int i = 0; i < APP_COUNT; i++) {
    lv_obj_t* btn = lv_btn_create(appsScreen);
    lv_obj_set_size(btn, 280, btnH);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0,
                 i * pitch - ((APP_COUNT - 1) * pitch) / 2);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x10101E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(APP_ENTRIES[i].color), 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%s  %s", APP_ENTRIES[i].icon, APP_ENTRIES[i].name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(APP_ENTRIES[i].color), 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, appsBtnCB, LV_EVENT_CLICKED, this);
  }

  lv_obj_t* hint = lv_label_create(appsScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(appsScreen, appsGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::showAppsMenu() {
  // Toggle: the same physical button opens and closes the menu.
  if (lv_scr_act() == appsScreen) {
    showPetScreen();
    return;
  }
  lv_scr_load_anim(appsScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::appsBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  // Swipes emit a CLICKED on release; same guard as the habit list.
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  switch (idx) {
    case 0:
      self->showWorkoutScreen();
      break;
    case 1:
      self->pomState = POM_IDLE;  // always start fresh, same as the swipe path
      self->showPomodoroScreen();
      break;
    default:
      break;
  }
}

void PetUI::appsGestureCB(lv_event_t* e) {
  // Opened by a physical button, so there's no "opposite swipe" to mirror:
  // any swipe direction dismisses back to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->showPetScreen();
}

/* ---- workout screen --------------------------------------------- */

// Applies the selected/unselected style to all three difficulty buttons.
// Called once at build time and again whenever the selection changes.
static void applyDiffStyles(lv_obj_t* btns[3], int selected) {
  // per-difficulty: border color, text color, bg when selected
  static const uint32_t SEL_BG[3]     = { 0x0F3820, 0x2A1C00, 0x3A0808 };
  static const uint32_t SEL_BORDER[3] = { 0x3EE8A0, 0xD4A030, 0xE85050 };
  static const uint32_t SEL_TEXT[3]   = { 0x3EE8A0, 0xFFD060, 0xFF8080 };
  static const uint32_t OFF_BG        = 0x0A0A0A;
  static const uint32_t OFF_BORDER[3] = { 0x1A3A25, 0x3A2A00, 0x3A1010 };
  static const uint32_t OFF_TEXT[3]   = { 0x2A6040, 0x5A4010, 0x6A2020 };

  for (int i = 0; i < 3; i++) {
    bool on = (i == selected);
    lv_obj_set_style_bg_color(btns[i], lv_color_hex(on ? SEL_BG[i] : OFF_BG), 0);
    lv_obj_set_style_border_color(btns[i], lv_color_hex(on ? SEL_BORDER[i] : OFF_BORDER[i]), 0);
    lv_obj_t* lbl = lv_obj_get_child(btns[i], 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(on ? SEL_TEXT[i] : OFF_TEXT[i]), 0);
  }
}

void PetUI::buildWorkoutScreen() {
  workoutScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(workoutScreen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(workoutScreen, 0, 0);
  lv_obj_clear_flag(workoutScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(workoutScreen);
  lv_label_set_text(title, "WORKOUT");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_set_style_text_color(title, lv_color_hex(0x3EE8A0), 0);

  // Difficulty selector — 3 buttons near the top, narrowed for the round
  // glass: 3×90px with 8px gaps centered (x=90..376) at y=56, where the
  // visible circle is ~288px wide.
  static const char* DIFF_LABELS[3] = { "Easy", "Medium", "Hard" };
  for (int i = 0; i < 3; i++) {
    lv_obj_t* btn = lv_btn_create(workoutScreen);
    lv_obj_set_size(btn, 90, 26);
    lv_obj_set_pos(btn, 90 + i * 98, 56);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, DIFF_LABELS[i]);
    // 90×26 buttons: "Medium" at the 20 pt default overflows, keep these small.
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, workoutDiffBtnCB, LV_EVENT_CLICKED, this);
    diffBtns[i] = btn;
  }
  applyDiffStyles(diffBtns, workoutDifficulty);

  // Main tap zone — starts below difficulty buttons (y=92)
  lv_obj_t* tapZone = lv_obj_create(workoutScreen);
  lv_obj_set_size(tapZone, 340, 150);
  lv_obj_set_pos(tapZone, 63, 92);
  lv_obj_set_style_bg_color(tapZone, lv_color_hex(0x080818), 0);
  lv_obj_set_style_bg_opa(tapZone, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tapZone, 14, 0);
  lv_obj_set_style_border_color(tapZone, lv_color_hex(0x2A2A6A), 0);
  lv_obj_set_style_border_width(tapZone, 2, 0);
  lv_obj_set_style_pad_all(tapZone, 0, 0);
  lv_obj_clear_flag(tapZone, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tapZone, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(tapZone, workoutTapCB, LV_EVENT_CLICKED, this);

  repLabel = lv_label_create(tapZone);
  lv_label_set_text(repLabel, "0");
  lv_obj_set_style_text_font(repLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(repLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(repLabel, LV_ALIGN_CENTER, 0, -18);

  lv_obj_t* repUnit = lv_label_create(tapZone);
  lv_label_set_text(repUnit, "REPS");
  lv_obj_set_style_text_color(repUnit, lv_color_hex(0x5080A0), 0);
  lv_obj_align(repUnit, LV_ALIGN_CENTER, 0, 18);

  workoutHint = lv_label_create(tapZone);
  lv_label_set_text(workoutHint, "press START");
  lv_obj_set_style_text_color(workoutHint, lv_color_hex(0x3A6050), 0);
  lv_obj_align(workoutHint, LV_ALIGN_BOTTOM_MID, 0, -10);

  // Timer label — below the tap zone (tap zone bottom = 92+150 = 242)
  timerLabel = lv_label_create(workoutScreen);
  lv_label_set_text(timerLabel, "00:00");
  lv_obj_set_style_text_font(timerLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(timerLabel, lv_color_hex(0x6898B8), 0);
  lv_obj_align(timerLabel, LV_ALIGN_TOP_MID, 0, 252);

  // Layout (round 466∅, pad=0):
  //   startBtn  165×44  pos(63,  300)
  //   doneBtn   165×44  pos(238, 300)
  //   backBtn   140×34  pos(163, 364)

  // START / PAUSE button
  lv_obj_t* startBtn = lv_btn_create(workoutScreen);
  lv_obj_set_size(startBtn, 165, 44);
  lv_obj_set_pos(startBtn, 63, 300);
  lv_obj_set_style_bg_color(startBtn, lv_color_hex(0x0F3820), 0);
  lv_obj_set_style_bg_opa(startBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(startBtn, 10, 0);
  lv_obj_set_style_border_color(startBtn, lv_color_hex(0x3EE8A0), 0);
  lv_obj_set_style_border_width(startBtn, 2, 0);
  workoutStartLabel = lv_label_create(startBtn);
  lv_label_set_text(workoutStartLabel, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(workoutStartLabel, lv_color_hex(0x3EE8A0), 0);
  lv_obj_center(workoutStartLabel);
  lv_obj_add_event_cb(startBtn, workoutStartBtnCB, LV_EVENT_CLICKED, this);

  // DONE button
  lv_obj_t* doneBtn = lv_btn_create(workoutScreen);
  lv_obj_set_size(doneBtn, 165, 44);
  lv_obj_set_pos(doneBtn, 238, 300);
  lv_obj_set_style_bg_color(doneBtn, lv_color_hex(0x301A00), 0);
  lv_obj_set_style_bg_opa(doneBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(doneBtn, 10, 0);
  lv_obj_set_style_border_color(doneBtn, lv_color_hex(0xD4A030), 0);
  lv_obj_set_style_border_width(doneBtn, 2, 0);
  workoutDoneLabel = lv_label_create(doneBtn);
  lv_label_set_text(workoutDoneLabel, LV_SYMBOL_OK "  DONE");
  lv_obj_set_style_text_color(workoutDoneLabel, lv_color_hex(0xFFD060), 0);
  lv_obj_center(workoutDoneLabel);
  lv_obj_add_event_cb(doneBtn, workoutDoneBtnCB, LV_EVENT_CLICKED, this);

  // Cancel / back button
  lv_obj_t* backBtn = lv_btn_create(workoutScreen);
  lv_obj_set_size(backBtn, 140, 34);
  lv_obj_set_pos(backBtn, 163, 364);
  lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x161630), 0);
  lv_obj_set_style_bg_opa(backBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(backBtn, 8, 0);
  lv_obj_set_style_border_color(backBtn, lv_color_hex(0x6060C0), 0);
  lv_obj_set_style_border_width(backBtn, 2, 0);
  lv_obj_t* backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Cancel");
  lv_obj_set_style_text_color(backLbl, lv_color_hex(0xA0A0F0), 0);
  lv_obj_center(backLbl);
  lv_obj_add_event_cb(backBtn, workoutBackBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* exitHint = lv_label_create(workoutScreen);
  lv_label_set_text(exitHint, "swipe " LV_SYMBOL_DOWN " for pet");
  lv_obj_align(exitHint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(exitHint, lv_color_hex(0x1A3028), 0);
  lv_obj_set_style_text_font(exitHint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(workoutScreen, workoutGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::showWorkoutScreen() {
  workoutReps = 0;
  workoutRunning = false;
  workoutElapsedMs = 0;
  if (workoutClockTimer) { lv_timer_del(workoutClockTimer); workoutClockTimer = nullptr; }

  lv_label_set_text(repLabel, "0");
  lv_label_set_text(timerLabel, "00:00");
  lv_label_set_text(workoutStartLabel, LV_SYMBOL_PLAY "  START");
  int target = WORKOUT_TARGETS[workoutDifficulty];
  lv_label_set_text_fmt(workoutDoneLabel, LV_SYMBOL_OK "  DONE (need %d)", target);
  lv_label_set_text(workoutHint, "press START");
  applyDiffStyles(diffBtns, workoutDifficulty);

  // Opened by swipe-up from the pet screen, so slide in from the bottom.
  lv_scr_load_anim(workoutScreen, LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, false);
}

void PetUI::addWorkoutRep() {
  if (!workoutRunning) return;
  workoutReps++;
  lv_label_set_text_fmt(repLabel, "%d", workoutReps);
  int target = WORKOUT_TARGETS[workoutDifficulty];
  if (workoutReps >= target) {
    lv_label_set_text_fmt(workoutDoneLabel, LV_SYMBOL_OK "  FEED! (%d/%d)", workoutReps, target);
  } else {
    lv_label_set_text_fmt(workoutDoneLabel, LV_SYMBOL_OK "  %d/%d", workoutReps, target);
  }
}

void PetUI::workoutTapCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->addWorkoutRep();
}

void PetUI::workoutStartBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (!self->workoutRunning) {
    self->workoutRunning = true;
    self->workoutStartMs = lv_tick_get();
    lv_label_set_text(self->workoutStartLabel, LV_SYMBOL_PAUSE "  PAUSE");
    lv_label_set_text(self->workoutHint, "tap here to count");
    lv_obj_set_style_text_color(self->workoutHint, lv_color_hex(0x2A4A3A), 0);
    if (!self->workoutClockTimer) {
      self->workoutClockTimer = lv_timer_create(workoutClockCB, 500, self);
    }
  } else {
    self->workoutElapsedMs += lv_tick_get() - self->workoutStartMs;
    self->workoutRunning = false;
    lv_label_set_text(self->workoutStartLabel, LV_SYMBOL_PLAY "  RESUME");
    lv_label_set_text(self->workoutHint, "paused");
    lv_obj_set_style_text_color(self->workoutHint, lv_color_hex(0x1A2A2A), 0);
  }
}

void PetUI::workoutDoneBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->workoutRunning = false;
  if (self->workoutClockTimer) { lv_timer_del(self->workoutClockTimer); self->workoutClockTimer = nullptr; }

  int target = WORKOUT_TARGETS[self->workoutDifficulty];
  if (self->workoutReps >= target) {
    // Meal size scales with difficulty: a Hard workout nearly fills the
    // belly, Easy is a snack that won't keep up with hunger decay alone.
    self->pet->feed(FEED_HUNGER[self->workoutDifficulty],
                    FEED_MOOD[self->workoutDifficulty]);
    self->refreshPetScreen();
    // Show "FED!" pill popup at screen center, lives on layer_top
    lv_obj_t* pill = lv_obj_create(lv_layer_top());
    lv_obj_set_size(pill, 130, 44);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x061A0E), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 22, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0x3EE8A0), 0);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(pill, 233 - 65, 233 - 22);
    lv_obj_t* lbl = lv_label_create(pill);
    lv_label_set_text_fmt(lbl, LV_SYMBOL_OK " FED +%d", FEED_HUNGER[self->workoutDifficulty]);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x3EE8A0), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    // Float up + fade out
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, pill);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
    lv_anim_set_values(&a, 211, 211 - 90);
    lv_anim_set_time(&a, 1400);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, xpPopupDeleteCB);
    lv_anim_set_user_data(&a, pill);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, pill);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 500);
    lv_anim_set_time(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
  }

  lv_scr_load_anim(self->habitScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 500, false);
  self->refreshHabitScreen();
}

void PetUI::workoutBackBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->workoutRunning = false;
  if (self->workoutClockTimer) { lv_timer_del(self->workoutClockTimer); self->workoutClockTimer = nullptr; }
  // Back to the pet screen (workout is entered from there by swipe-up).
  lv_scr_load_anim(self->petScreen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
  self->refreshPetScreen();
}

void PetUI::workoutClockCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (!self->workoutRunning) return;
  uint32_t totalMs = self->workoutElapsedMs + (lv_tick_get() - self->workoutStartMs);
  uint32_t secs    = totalMs / 1000;
  lv_label_set_text_fmt(self->timerLabel, "%02d:%02d", secs / 60, secs % 60);
}

void PetUI::workoutDiffBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (self->workoutRunning) return;  // don't change difficulty mid-workout
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  self->workoutDifficulty = (WorkoutDifficulty)idx;
  applyDiffStyles(self->diffBtns, idx);
  // Reset done label to show new target
  int target = WORKOUT_TARGETS[idx];
  lv_label_set_text_fmt(self->workoutDoneLabel, LV_SYMBOL_OK "  DONE (need %d)", target);
}

/* ---- pomodoro screen --------------------------------------------- */

#ifdef POMODORO_SIM_MODE
static const uint32_t POMODORO_FOCUS_MS = 30 * 1000;
static const uint32_t POMODORO_BREAK_MS = 10 * 1000;
#else
static const uint32_t POMODORO_FOCUS_MS = 25 * 60 * 1000;
static const uint32_t POMODORO_BREAK_MS = 5  * 60 * 1000;
#endif
static const int POMODORO_XP = 25;

void PetUI::buildPomodoroScreen() {
  pomodoroScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(pomodoroScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(pomodoroScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(pomodoroScreen);
  lv_label_set_text(title, "FOCUS TIMER");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_set_style_text_color(title, lv_color_hex(0x804020), 0);

  // Ring arc — PART_MAIN = dark track, PART_INDICATOR = countdown progress
  pomArc = lv_arc_create(pomodoroScreen);
  lv_obj_set_size(pomArc, 320, 320);  // shrunk so start/cancel fit below inside the round glass
  lv_obj_align(pomArc, LV_ALIGN_CENTER, 0, -24);
  lv_arc_set_rotation(pomArc, 270);        // visual start at 12 o'clock
  lv_arc_set_bg_angles(pomArc, 0, 359);   // full circle track (359° avoids wrap glitch)
  lv_arc_set_range(pomArc, 0, 1000);
  lv_arc_set_value(pomArc, 1000);          // full ring at start

  lv_obj_set_style_arc_color(pomArc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
  lv_obj_set_style_arc_width(pomArc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_color(pomArc, lv_color_hex(0xE87030), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(pomArc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_opa(pomArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(pomArc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(pomArc, LV_OBJ_FLAG_CLICKABLE);

  // Countdown display inside the ring
  pomTimeLabel = lv_label_create(pomodoroScreen);
  lv_label_set_text(pomTimeLabel, "25:00");
  lv_obj_set_style_text_font(pomTimeLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(pomTimeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(pomTimeLabel, LV_ALIGN_CENTER, 0, -20);

  pomModeLabel = lv_label_create(pomodoroScreen);
  lv_label_set_text(pomModeLabel, "FOCUS");
  lv_obj_set_style_text_color(pomModeLabel, lv_color_hex(0x604020), 0);
  lv_obj_align(pomModeLabel, LV_ALIGN_CENTER, 0, 18);

  pomBlockLabel = lv_label_create(pomodoroScreen);
  lv_label_set_text(pomBlockLabel, "");
  lv_obj_set_style_text_color(pomBlockLabel, lv_color_hex(0x503010), 0);
  lv_obj_align(pomBlockLabel, LV_ALIGN_CENTER, 0, 50);

  // Play/pause is a double-tap anywhere on the screen; this label doubles
  // as the instruction text and reflects the current state.
  pomStartLabel = lv_label_create(pomodoroScreen);
  lv_label_set_text(pomStartLabel, LV_SYMBOL_PLAY "  double tap to start");
  lv_obj_set_style_text_color(pomStartLabel, lv_color_hex(0xE87030), 0);
  lv_obj_align(pomStartLabel, LV_ALIGN_BOTTOM_MID, 0, -76);

  // CANCEL button (explicit abort; double-tap only toggles play/pause)
  lv_obj_t* cancelBtn = lv_btn_create(pomodoroScreen);
  lv_obj_set_size(cancelBtn, 130, 40);  // fits the LV_SYMBOL_CLOSE " Cancel" label at 20 pt
  lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_MID, 0, -36);
  lv_obj_set_style_bg_color(cancelBtn, lv_color_hex(0x161630), 0);
  lv_obj_set_style_bg_opa(cancelBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(cancelBtn, 10, 0);
  lv_obj_set_style_border_color(cancelBtn, lv_color_hex(0x6060C0), 0);
  lv_obj_set_style_border_width(cancelBtn, 2, 0);
  lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
  lv_label_set_text(cancelLbl, LV_SYMBOL_CLOSE " Cancel");
  lv_obj_set_style_text_color(cancelLbl, lv_color_hex(0xA0A0F0), 0);
  lv_obj_center(cancelLbl);
  lv_obj_add_event_cb(cancelBtn, pomCancelBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* exitHint = lv_label_create(pomodoroScreen);
  lv_label_set_text(exitHint, "swipe " LV_SYMBOL_UP " for pet");
  lv_obj_align(exitHint, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_style_text_color(exitHint, lv_color_hex(0x3A2410), 0);
  // Bottom edge of the circle is only ~148 px wide at this y; 20 pt clips.
  lv_obj_set_style_text_font(exitHint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(pomodoroScreen, pomGestureCB, LV_EVENT_GESTURE, this);
  lv_obj_add_flag(pomodoroScreen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(pomodoroScreen, pomTapCB, LV_EVENT_CLICKED, this);
}

// Double-tap anywhere on the focus screen toggles play/pause. Swipe releases
// also emit CLICKED, so anything right after a gesture is discarded.
void PetUI::pomTapCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  uint32_t now = lv_tick_get();
  if (now - self->lastGestureMs < 600) {
    self->pomLastTapMs = 0;
    return;
  }
  if (self->pomLastTapMs != 0 && now - self->pomLastTapMs < 400) {
    self->pomLastTapMs = 0;
    pomStartBtnCB(e);
  } else {
    self->pomLastTapMs = now;
  }
}

// Updates arc value, time label, mode label, and block dots from current state.
void PetUI::refreshPomodoroRing() {
  bool isFocus = (pomState == POM_FOCUS);

  lv_color_t col = isFocus ? lv_color_hex(0xE87030) : lv_color_hex(0x3EE8A0);
  lv_obj_set_style_arc_color(pomArc, col, LV_PART_INDICATOR);
  lv_arc_set_value(pomArc, 1000);  // reset to full for new interval

  uint32_t blockMs   = isFocus ? POMODORO_FOCUS_MS : POMODORO_BREAK_MS;
  uint32_t totalSecs = blockMs / 1000;
  lv_label_set_text_fmt(pomTimeLabel, "%02u:%02u", totalSecs / 60, totalSecs % 60);
  lv_obj_set_style_text_color(pomTimeLabel, lv_color_hex(0xFFFFFF), 0);

  if (pomState == POM_IDLE) {
    lv_label_set_text(pomModeLabel, "FOCUS");
    lv_obj_set_style_text_color(pomModeLabel, lv_color_hex(0x604020), 0);
  } else if (isFocus) {
    lv_label_set_text(pomModeLabel, "FOCUS");
    lv_obj_set_style_text_color(pomModeLabel, lv_color_hex(0xE87030), 0);
  } else {
    lv_label_set_text(pomModeLabel, "BREAK");
    lv_obj_set_style_text_color(pomModeLabel, lv_color_hex(0x3EE8A0), 0);
  }

  // Block progress: "✓ ✓ ─ ─" (4 slots)
  char dots[64] = {};
  int pos = 0;
  for (int i = 0; i < 4; i++) {
    if (i > 0) { dots[pos++] = ' '; }
    const char* sym = (i < pomBlocks) ? LV_SYMBOL_OK : LV_SYMBOL_MINUS;
    for (const char* c = sym; *c; c++) dots[pos++] = *c;
  }
  dots[pos] = '\0';
  lv_label_set_text(pomBlockLabel, dots);
  lv_obj_set_style_text_color(pomBlockLabel,
    isFocus ? lv_color_hex(0x804020) : lv_color_hex(0x30806A), 0);
}

void PetUI::showPomodoroScreen() {
  // Reset to fresh focus state; keep block count for session continuity
  pomRunning    = false;
  pomElapsedMs  = 0;
  pomStartMs    = 0;
  if (pomClockTimer) { lv_timer_del(pomClockTimer); pomClockTimer = nullptr; }

  // On first open: POM_IDLE → show default countdown, dimmed labels
  // On re-open mid-session: preserve pomState/pomBlocks
  if (pomState == POM_IDLE) {
    pomBlocks = 0;
    uint32_t totalSecs = POMODORO_FOCUS_MS / 1000;
    lv_label_set_text_fmt(pomTimeLabel, "%02u:%02u", totalSecs / 60, totalSecs % 60);
    lv_obj_set_style_arc_color(pomArc, lv_color_hex(0xE87030), LV_PART_INDICATOR);
    lv_arc_set_value(pomArc, 1000);
    lv_label_set_text(pomModeLabel, "FOCUS");
    lv_obj_set_style_text_color(pomModeLabel, lv_color_hex(0x604020), 0);
    lv_label_set_text(pomBlockLabel, "");
  }

  lv_label_set_text(pomStartLabel, LV_SYMBOL_PLAY "  double tap to start");
  // Opened by swipe-down from the pet screen, so slide in from the top.
  lv_scr_load_anim(pomodoroScreen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
}

void PetUI::pomStartBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);

  if (self->pomState == POM_IDLE) {
    // First START — begin a focus block
    self->pomState    = POM_FOCUS;
    self->pomRunning  = true;
    self->pomElapsedMs = 0;
    self->pomStartMs  = lv_tick_get();
    self->refreshPomodoroRing();
    lv_label_set_text(self->pomStartLabel, LV_SYMBOL_PAUSE "  double tap to pause");
    if (!self->pomClockTimer)
      self->pomClockTimer = lv_timer_create(pomClockCB, 500, self);
    return;
  }

  if (self->pomRunning) {
    // Pause
    self->pomElapsedMs += lv_tick_get() - self->pomStartMs;
    self->pomRunning = false;
    lv_label_set_text(self->pomStartLabel, LV_SYMBOL_PLAY "  double tap to resume");
  } else {
    // Resume
    self->pomStartMs = lv_tick_get();
    self->pomRunning = true;
    lv_label_set_text(self->pomStartLabel, LV_SYMBOL_PAUSE "  double tap to pause");
  }
}

void PetUI::pomCancelBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->pomRunning = false;
  self->pomState   = POM_IDLE;
  if (self->pomClockTimer) { lv_timer_del(self->pomClockTimer); self->pomClockTimer = nullptr; }
  // Back to the pet screen (focus is entered from there by swipe-down).
  lv_scr_load_anim(self->petScreen, LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, false);
  self->refreshPetScreen();
}

void PetUI::pomClockCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (!self->pomRunning) return;

  uint32_t totalMs = self->pomElapsedMs + (lv_tick_get() - self->pomStartMs);
  uint32_t blockMs = (self->pomState == POM_FOCUS) ? POMODORO_FOCUS_MS : POMODORO_BREAK_MS;

  if (totalMs >= blockMs) {
    // Block complete
    self->pomRunning  = false;
    self->pomElapsedMs = 0;
    self->pomStartMs   = lv_tick_get();

    if (self->pomState == POM_FOCUS) {
      self->pomBlocks++;

      int stageBefore = self->pet->getStage();
      int levelBefore = self->pet->getLevel();
      self->pet->addXP(POMODORO_XP);
      bool evolved = self->pet->getStage() > stageBefore;

      // XP popup centered on the arc
      lv_area_t area;
      lv_obj_get_coords(self->pomArc, &area);
      self->showXpPopup(area, POMODORO_XP);
      if (evolved) self->showEvolutionBurst();
      else if (self->pet->getLevel() > levelBefore)
        self->showLevelUpPill(self->pet->getLevel());

      // Transition to break
      self->pomState = POM_BREAK;
    } else {
      // Break over — next focus block; auto-arm but wait for START
      self->pomState = POM_FOCUS;
    }

    self->refreshPomodoroRing();
    // Auto-start the next interval
    self->pomRunning  = true;
    self->pomStartMs  = lv_tick_get();
    lv_label_set_text(self->pomStartLabel, LV_SYMBOL_PAUSE "  double tap to pause");
    return;
  }

  // Update ring + countdown display
  uint32_t remaining = blockMs - totalMs;
  int permille = (int)(remaining * 1000 / blockMs);
  lv_arc_set_value(self->pomArc, permille);

  uint32_t secs = (remaining + 999) / 1000;  // ceil to avoid "0:00" flash early
  lv_label_set_text_fmt(self->pomTimeLabel, "%02u:%02u", secs / 60, secs % 60);
}

void PetUI::pomodoroGuiltTrip() {
  if (pomState != POM_FOCUS || !pomRunning) return;
  // Flash arc red and scold the user for picking up the device mid-focus
  lv_obj_set_style_arc_color(pomArc, lv_color_hex(0xFF3030), LV_PART_INDICATOR);
  lv_label_set_text(pomModeLabel, LV_SYMBOL_WARNING " FOCUS!");
  lv_obj_set_style_text_color(pomModeLabel, lv_color_hex(0xFF3030), 0);
  lv_timer_t* rv = lv_timer_create(pomGuiltRevertCB, 1500, this);
  lv_timer_set_repeat_count(rv, 1);
}

void PetUI::sedentaryNudge() {
  if (!sedentaryActive) {
    sedentaryActive = true;
    pet->setMood(LV_MAX(0, pet->getMood() - SEDENTARY_MOOD_KNOCK));
    applyMoodExpression();
  }
  showSedentaryBubble();
}

void PetUI::showSedentaryBubble() {
  lv_obj_t* bubble = lv_obj_create(lv_layer_top());
  lv_obj_set_size(bubble, 210, 36);
  lv_obj_set_style_bg_color(bubble, lv_color_hex(0x0D1A12), 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bubble, 18, 0);
  lv_obj_set_style_border_color(bubble, lv_color_hex(0x1E4A28), 0);
  lv_obj_set_style_border_width(bubble, 1, 0);
  lv_obj_set_style_pad_all(bubble, 0, 0);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(bubble);
  lv_label_set_text(lbl, "I want a stretch...");
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x3EE8A0), 0);
  lv_obj_center(lbl);

  lv_coord_t cx     = lv_obj_get_width(petScreen) / 2 - 105;
  lv_coord_t startY = 340;
  lv_coord_t endY   = 275;
  lv_obj_set_pos(bubble, cx, startY);

  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, bubble);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, startY, endY);
  lv_anim_set_time(&a, 3000);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, bubble);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, 1500);
  lv_anim_set_time(&a, 1500);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, xpPopupDeleteCB);
  lv_anim_set_user_data(&a, bubble);
  lv_anim_start(&a);
}

void PetUI::updateBattery(int pct, bool charging) {
  batteryPct = pct;
  if (pct < 0) {
    lv_obj_add_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN);

  lv_color_t col;
  const char* sym;
  if (charging) {
    col = lv_color_hex(0xFFA030); sym = LV_SYMBOL_CHARGE;
  } else if (pct >= 50) {
    col = lv_color_hex(0x3EE8A0); sym = LV_SYMBOL_CHARGE;
  } else if (pct >= 20) {
    col = lv_color_hex(0xFFB84C); sym = LV_SYMBOL_CHARGE;
  } else if (pct >= 10) {
    col = lv_color_hex(0xFF7730); sym = LV_SYMBOL_WARNING;
  } else {
    col = lv_color_hex(0xFF3030); sym = LV_SYMBOL_WARNING;
  }
  lv_obj_set_style_text_color(batteryLabel, col, 0);
  lv_label_set_text_fmt(batteryLabel, "%s %d%%", sym, pct);
  lv_obj_align(batteryLabel, LV_ALIGN_BOTTOM_MID, 0, -82);  // re-align after text resize

  if (pct >= 20 || charging) {
    batteryWarnShown = false;   // recovered; re-arm the low-battery warning
  }
  if (pct < 15 && !charging && !batteryWarnShown) {
    batteryWarnShown = true;
    showSleepyBubble();
  }
}

void PetUI::showSleepyBubble() {
  lv_obj_t* bubble = lv_obj_create(lv_layer_top());
  lv_obj_set_size(bubble, 220, 36);
  lv_obj_set_style_bg_color(bubble, lv_color_hex(0x1A0E00), 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bubble, 18, 0);
  lv_obj_set_style_border_color(bubble, lv_color_hex(0x4A2800), 0);
  lv_obj_set_style_border_width(bubble, 1, 0);
  lv_obj_set_style_pad_all(bubble, 0, 0);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(bubble);
  lv_label_set_text(lbl, LV_SYMBOL_WARNING " getting sleepy...");
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFB84C), 0);
  lv_obj_center(lbl);

  lv_coord_t cx     = lv_obj_get_width(petScreen) / 2 - 110;
  lv_coord_t startY = 340;
  lv_coord_t endY   = 275;
  lv_obj_set_pos(bubble, cx, startY);

  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, bubble);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, startY, endY);
  lv_anim_set_time(&a, 3000);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, bubble);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, 1500);
  lv_anim_set_time(&a, 1500);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, xpPopupDeleteCB);
  lv_anim_set_user_data(&a, bubble);
  lv_anim_start(&a);
}

void PetUI::sedentaryCheckCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  uint32_t idle = lv_disp_get_inactive_time(NULL);
  if (idle >= SEDENTARY_IDLE_MS && !self->sedentaryActive) {
    self->sedentaryNudge();
  } else if (self->sedentaryActive && idle < 15000) {
    // Recent touch detected — restore normal state
    self->sedentaryActive = false;
    self->applyMoodExpression();
  }
}

void PetUI::pomGuiltRevertCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (self->pomState != POM_FOCUS) return;
  lv_obj_set_style_arc_color(self->pomArc, lv_color_hex(0xE87030), LV_PART_INDICATOR);
  lv_label_set_text(self->pomModeLabel, "FOCUS");
  lv_obj_set_style_text_color(self->pomModeLabel, lv_color_hex(0xE87030), 0);
}
