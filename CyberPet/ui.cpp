#include "ui.h"
#include <cstdlib>

// Stage colors - blob visually changes as it evolves
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
  blobBaseX = blobBaseY = -1;
  eyeDriftX = eyeDriftY = 0;
  workoutReps = 0; workoutRunning = false;
  workoutDifficulty = DIFF_MEDIUM;
  workoutElapsedMs = 0; workoutClockTimer = nullptr;
  srand((unsigned)lv_tick_get() ^ 0xBEEF);
  buildPetScreen();
  buildHabitScreen();
  buildWorkoutScreen();
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
  lv_obj_align(moodBar, LV_ALIGN_TOP_RIGHT, -20, 44);
  lv_bar_set_range(moodBar, 0, 100);
  lv_obj_set_style_bg_color(moodBar, lv_color_hex(0x0E0E1C), 0);
  lv_obj_set_style_bg_opa(moodBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(moodBar, lv_color_hex(0x3ED9AA), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(moodBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(moodBar, 5, 0);
  lv_obj_set_style_radius(moodBar, 5, LV_PART_INDICATOR);

  // --- hunger corner (top-left, centered in a 90px column at x=15..105) ---
  lv_obj_t* hungerCaption = lv_label_create(petScreen);
  lv_label_set_text(hungerCaption, "hunger");
  lv_obj_set_pos(hungerCaption, 15, 10);
  lv_obj_set_width(hungerCaption, 90);
  lv_obj_set_style_text_align(hungerCaption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(hungerCaption, lv_color_hex(0x2A4A35), 0);

  hungerLabel = lv_label_create(petScreen);
  lv_obj_set_pos(hungerLabel, 15, 26);
  lv_obj_set_width(hungerLabel, 90);
  lv_obj_set_style_text_align(hungerLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(hungerLabel, lv_color_hex(0x3EE8A0), 0);

  hungerBar = lv_bar_create(petScreen);
  lv_obj_set_size(hungerBar, 80, 6);
  lv_obj_set_pos(hungerBar, 20, 44);
  lv_bar_set_range(hungerBar, 0, 100);
  lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0x0E0E1C), 0);
  lv_obj_set_style_bg_opa(hungerBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(hungerBar, lv_color_hex(0x3EE8A0), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(hungerBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(hungerBar, 5, 0);
  lv_obj_set_style_radius(hungerBar, 5, LV_PART_INDICATOR);

  // --- mood corner (top-right, mirrored 90px column at x=361..451) ---
  lv_obj_t* moodCaption = lv_label_create(petScreen);
  lv_label_set_text(moodCaption, "mood");
  lv_obj_set_width(moodCaption, 90);
  lv_obj_align(moodCaption, LV_ALIGN_TOP_RIGHT, -15, 10);
  lv_obj_set_style_text_align(moodCaption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(moodCaption, lv_color_hex(0x2A4A35), 0);

  moodLabel = lv_label_create(petScreen);
  lv_obj_set_width(moodLabel, 90);
  lv_obj_align(moodLabel, LV_ALIGN_TOP_RIGHT, -15, 26);
  lv_obj_set_style_text_align(moodLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(moodLabel, lv_color_hex(0x3EE8A0), 0);

  lv_obj_add_flag(petScreen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(petScreen, goToHabitsEventCB, LV_EVENT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(petScreen);
  lv_label_set_text(hint, "Tap to view habits");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);

  exprTimer  = lv_timer_create(exprTimerCB,  3000, this);
  blinkTimer = lv_timer_create(blinkTimerCB, 4500, this);
}

void PetUI::buildHabitScreen() {
  habitScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(habitScreen, lv_color_hex(0x000000), 0);

  lv_obj_t* title = lv_label_create(habitScreen);
  lv_label_set_text(title, "Today's Habits");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_style_text_color(title, lv_color_hex(0x9090CC), 0);

  habitList = lv_list_create(habitScreen);
  lv_obj_set_size(habitList, 420, 310);
  lv_obj_align(habitList, LV_ALIGN_CENTER, 0, -10);
  lv_obj_set_style_bg_color(habitList, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(habitList, lv_color_hex(0x1A1A2E), 0);
  lv_obj_set_style_border_width(habitList, 1, 0);
  lv_obj_set_style_pad_row(habitList, 6, 0);
  lv_obj_set_style_pad_all(habitList, 8, 0);

  lv_obj_t* backBtn = lv_btn_create(habitScreen);
  lv_obj_set_size(backBtn, 180, 38);
  lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, -100, -10);
  lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x0D0D22), 0);
  lv_obj_set_style_bg_opa(backBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(backBtn, lv_color_hex(0x2A2A66), 0);
  lv_obj_set_style_border_width(backBtn, 1, 0);
  lv_obj_t* backLabel = lv_label_create(backBtn);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Pet");
  lv_obj_set_style_text_color(backLabel, lv_color_hex(0x6868AA), 0);
  lv_obj_center(backLabel);
  lv_obj_add_event_cb(backBtn, goToPetEventCB, LV_EVENT_CLICKED, this);

  lv_obj_t* workoutBtn = lv_btn_create(habitScreen);
  lv_obj_set_size(workoutBtn, 180, 38);
  lv_obj_align(workoutBtn, LV_ALIGN_BOTTOM_MID, 100, -10);
  lv_obj_set_style_bg_color(workoutBtn, lv_color_hex(0x142A1C), 0);
  lv_obj_set_style_bg_opa(workoutBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(workoutBtn, lv_color_hex(0x3EE8A0), 0);
  lv_obj_set_style_border_width(workoutBtn, 2, 0);
  lv_obj_t* workoutBtnLbl = lv_label_create(workoutBtn);
  lv_label_set_text(workoutBtnLbl, LV_SYMBOL_CHARGE " Workout");
  lv_obj_set_style_text_color(workoutBtnLbl, lv_color_hex(0x3EE8A0), 0);
  lv_obj_center(workoutBtnLbl);
  lv_obj_add_event_cb(workoutBtn, workoutGoToCB, LV_EVENT_CLICKED, this);
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
  lv_obj_set_style_bg_color(blobShape, stageColor(stage), 0);

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
    lv_obj_align(moodLabel, LV_ALIGN_TOP_RIGHT, -15, 26);
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
  if (mood >= 70) {
    ew = eyeBaseW * 6 / 5; eh = ew;
  } else if (mood >= 30) {
    ew = eyeBaseW; eh = ew;
  } else {
    ew = eyeBaseW * 5 / 4;
    eh = LV_MAX(3, eyeBaseW / 4);
  }
  // Shift eyes in the direction the blob is moving (cleared on landing)
  placeEye(eyeLeft,  -eyeOffX + eyeDriftX, eyeOffY + eyeDriftY, ew, eh);
  placeEye(eyeRight,  eyeOffX + eyeDriftX, eyeOffY + eyeDriftY, ew, eh);
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

    char buf[48];
    snprintf(buf, sizeof(buf), "%s  +%dxp  streak %d", h->name, h->xpValue, h->streak);

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

void PetUI::habitButtonEventCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  lv_obj_t* btn = lv_event_get_target(e);
  int index = (int)(intptr_t)lv_obj_get_user_data(btn);
  if (self->tracker->completeHabit(index)) {
    Habit* h = self->tracker->get(index);
    self->pet->addXP(h->xpValue);

    // Capture screen coords before refreshHabitScreen() destroys btn
    lv_area_t coords;
    lv_obj_get_coords(btn, &coords);
    int xpVal = h->xpValue;

    self->refreshHabitScreen();
    self->showXpPopup(coords, xpVal);
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
  snprintf(buf, sizeof(buf), "+%d xp", xpValue);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFD166), 0);
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

void PetUI::goToHabitsEventCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->showHabitScreen();
}

void PetUI::goToPetEventCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
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
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_text_color(title, lv_color_hex(0x3EE8A0), 0);

  // Difficulty selector — 3 equal buttons across the top
  // Screen 466px wide, 3 buttons of 140px with 8px gaps: (466-3*140-2*8)/2=15 margin
  static const char* DIFF_LABELS[3] = { "Easy", "Medium", "Hard" };
  for (int i = 0; i < 3; i++) {
    lv_obj_t* btn = lv_btn_create(workoutScreen);
    lv_obj_set_size(btn, 140, 26);
    lv_obj_set_pos(btn, 15 + i * 148, 30);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, DIFF_LABELS[i]);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, workoutDiffBtnCB, LV_EVENT_CLICKED, this);
    diffBtns[i] = btn;
  }
  applyDiffStyles(diffBtns, workoutDifficulty);

  // Main tap zone — starts below difficulty buttons (y=64)
  lv_obj_t* tapZone = lv_obj_create(workoutScreen);
  lv_obj_set_size(tapZone, 430, 196);
  lv_obj_set_pos(tapZone, 18, 64);
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

  // Timer label — below the tap zone (tap zone bottom = 64+196 = 260)
  timerLabel = lv_label_create(workoutScreen);
  lv_label_set_text(timerLabel, "00:00");
  lv_obj_set_style_text_font(timerLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(timerLabel, lv_color_hex(0x6898B8), 0);
  lv_obj_align(timerLabel, LV_ALIGN_TOP_MID, 0, 268);

  // Layout (466×466, pad=0):
  //   startBtn  185×46  pos(38,  306)
  //   doneBtn   185×46  pos(243, 306)
  //   backBtn   140×34  pos(163, 364)

  // START / PAUSE button
  lv_obj_t* startBtn = lv_btn_create(workoutScreen);
  lv_obj_set_size(startBtn, 185, 46);
  lv_obj_set_pos(startBtn, 38, 306);
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
  lv_obj_set_size(doneBtn, 185, 46);
  lv_obj_set_pos(doneBtn, 243, 306);
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

  lv_scr_load_anim(workoutScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
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

void PetUI::workoutGoToCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->showWorkoutScreen();
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
    self->pet->feed();
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
    lv_label_set_text(lbl, LV_SYMBOL_OK " FED!");
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
  lv_scr_load_anim(self->habitScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
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
