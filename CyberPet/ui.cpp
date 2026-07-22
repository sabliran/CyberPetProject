#include "ui.h"
#include "sprites.h"
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
    case STAGE_BLOB:     return 165;
    case STAGE_CREATURE: return 205;
    case STAGE_EVOLVED:  return 245;
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

// Square eyes (radius 0) to match the pixel-art sprites — the old round
// eyes looked airbrushed against the chunky body pixels. All the blink /
// squint animations shrink width/height, which stays crisp on rectangles.
static void makeCircle(lv_obj_t* obj, lv_color_t color) {
  lv_obj_set_style_radius(obj, 0, 0);
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
// Image zoom, 256 = 100%. Safe on lv_img (the v8 transform trap is plain
// objects only). Scales the sprite around its center pivot; child eyes are
// NOT scaled — acceptable for the subtle puff/squash amounts used here.
static void animSetZoom(void* obj, int32_t v) { lv_img_set_zoom((lv_obj_t*)obj, (uint16_t)v); }

// Announce the zoom-puff overflow as extra draw area. Without this LVGL only
// erases the widget box when the blob moves, leaving ghost copies of the
// overflowing zoomed pixels behind.
static void blobExtDrawCB(lv_event_t* e) {
  // Must cover the deepest zoom-in (129%) plus the pat puff on top of it
  // (112% of that ≈ 145%): 245 px sprite * 45% / 2 ≈ 55 px of overflow.
  lv_event_set_ext_draw_size(e, 64);
}

// Body sprite per evolution stage (pixel art, body only — eyes on top).
static const lv_img_dsc_t* stageSprite(int stage) {
  switch (stage) {
    case STAGE_EGG:      return &sprite_egg;
    case STAGE_BLOB:     return &sprite_blob;
    case STAGE_CREATURE: return &sprite_creature;
    default:             return &sprite_evolved;
  }
}

// Walk frames: the animation alternates left-foot-up / right-foot-up so
// both legs step. The egg has no legs and glides on its single frame.
// The body sway in the walk art trails the movement, so rightward glides
// use the body-mirrored r frames; glides that shrink the depth zoom show
// the back view (the head is identical within each view).
static const lv_img_dsc_t* stageWalkSprite(int stage, bool rightFoot,
                                           bool movingRight, bool movingAway) {
  // [stage][away][mirrored][frame]
  static const lv_img_dsc_t* const T[3][2][2][2] = {
    {{{&sprite_blob_walk1,      &sprite_blob_walk2},
      {&sprite_blob_walk1r,     &sprite_blob_walk2r}},
     {{&sprite_blob_back1,      &sprite_blob_back2},
      {&sprite_blob_back1r,     &sprite_blob_back2r}}},
    {{{&sprite_creature_walk1,  &sprite_creature_walk2},
      {&sprite_creature_walk1r, &sprite_creature_walk2r}},
     {{&sprite_creature_back1,  &sprite_creature_back2},
      {&sprite_creature_back1r, &sprite_creature_back2r}}},
    {{{&sprite_evolved_walk1,   &sprite_evolved_walk2},
      {&sprite_evolved_walk1r,  &sprite_evolved_walk2r}},
     {{&sprite_evolved_back1,   &sprite_evolved_back2},
      {&sprite_evolved_back1r,  &sprite_evolved_back2r}}},
  };
  if (stage == STAGE_EGG) return &sprite_egg;
  int s = (stage == STAGE_BLOB) ? 0 : (stage == STAGE_CREATURE) ? 1 : 2;
  return T[s][movingAway][movingRight][rightFoot];
}
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

// Current depth zoom (256 = 100%), mirrored here so placeEye can scale the
// eyes with the body: image zoom only scales the bitmap, not child objects.
static int g_depthZoom = 256;

// Eye dimensions snap to the current sprite's pixel grid so every expression
// (squint, surprise, blink) stays chunky pixel-art instead of smoothly
// interpolated. A closed-eye blink becomes a one-sprite-pixel bar.
// The Smol sprites are the same 32x40 art at 2x/3x/4x per stage, so the
// grid is stage-dependent (set in refreshPetScreen alongside the eye layout).
// Everything scales by the depth zoom so the eyes shrink/grow (and stay on
// the TV screen) as the pet wanders nearer or farther.
static int g_eyeGrid = 2;

static void placeEye(lv_obj_t* eye, lv_coord_t cx, lv_coord_t cy, int w, int h) {
  cx = cx * g_depthZoom / 256;
  cy = cy * g_depthZoom / 256;
  w  = w  * g_depthZoom / 256;
  h  = h  * g_depthZoom / 256;
  w = LV_MAX(g_eyeGrid, (w / g_eyeGrid) * g_eyeGrid);
  h = LV_MAX(g_eyeGrid, (h / g_eyeGrid) * g_eyeGrid);
  lv_obj_set_size(eye, w, h);
  lv_obj_align(eye, LV_ALIGN_CENTER, cx, cy);
}

/* ------------------------------------------------------------------ */

void PetUI::init(Pet* petPtr, HabitTracker* trackerPtr) {
  pet      = petPtr;
  tracker  = trackerPtr;
  roamTimer = nullptr;
  blinkTimer = nullptr;
  walkAnimTimer = nullptr; walkFrameB = false; walkMirrored = false; walkAway = false;
  depthZoom = 256; g_depthZoom = 256;
  sedentaryActive  = false;
  sedentaryTimer   = nullptr;
  batteryPct       = -1;
  batteryWarnShown = false;
  blobBaseX = blobBaseY = -1;
  eyeDriftX = eyeDriftY = 0;
  pomState = POM_IDLE; pomLastTapMs = 0; pomRunning = false;
  pomBlocks = 0; pomStartMs = 0; pomElapsedMs = 0; pomClockTimer = nullptr;
  questCount = 0; goalCount = 0; trophyCount = 0;
  walkSteps = 0; walkSensorOk = false;
  backReps = 0; backRunning = false;
  pullupReps = 0; pullupRunning = false;
  cleanRunning = false; cleanStartMs = 0; cleanClockTimer = nullptr;
  hangStage = 0; hangRunning = false; hangStartMs = 0; hangClockTimer = nullptr;
  hangPassed[0] = hangPassed[1] = hangPassed[2] = false;
  plankRunning = false; plankStartMs = 0; plankLastMs = 0; plankBestMs = 0;
  plankClockTimer = nullptr;
  sitRunning = false; sitAlerting = false; sitIntervalMin = 20;
  sitStartMs = 0; sitLastChimeMs = 0; sitClockTimer = nullptr;
  medRunning = false; medDarkened = false; medIntervalMin = 10;
  medStartMs = 0; medClockTimer = nullptr;
  devSettings = { 100, 0, 200, 0, 2, 1, 1 };  // volume, theme, brightness, bg, sleep-min, steps-on, lift-wake
  pushReps = 0; pushRunning = false; lastPushTapMs = 0;
  squatReps = 0; squatRunning = false; lastSquatTapMs = 0;
  sleepLogged = false; sleepQuality = 0;
  lastGestureMs = 0; lastPatMs = 0; syncRequested = false;
  syncOverlay = nullptr; syncLabel = nullptr; syncSpinner = nullptr;
  syncTimeoutTimer = nullptr;
  srand((unsigned)lv_tick_get() ^ 0xBEEF);
  buildPetScreen();
  buildHabitScreen();
  buildQuestScreen();
  buildGoalScreen();
  buildAppsScreen();
  buildBackScreen();
  buildPullupScreen();
  buildCleanScreen();
  buildHangScreen();
  buildPlankScreen();
  buildSitScreen();
  buildMedScreen();
  buildSettingsScreen();
  buildPushScreen();
  buildSquatScreen();
  buildTrophyScreen();
  buildSleepScreen();
  buildWalkScreen();
  buildPomodoroScreen();
  sedentaryTimer = lv_timer_create(sedentaryCheckCB, SEDENTARY_CHECK_MS, this);

  lv_scr_load(petScreen);
  refreshPetScreen();
}

void PetUI::buildPetScreen() {
  petScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(petScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(petScreen, LV_OBJ_FLAG_SCROLLABLE);

  // Pixel-art body sprite (refreshPetScreen sets the per-stage source).
  // The round radius style is only for the glow shadow's shape; images
  // need CLICKABLE re-added (lv_img default is non-clickable, which would
  // silently kill the pat reaction) and OVERFLOW_VISIBLE so zoom puffs can
  // draw past the widget box.
  blobShape = lv_img_create(petScreen);
  lv_obj_set_pos(blobShape, 0, 0);  // roaming sets real position via lv_obj_set_pos in refreshPetScreen
  lv_obj_set_style_radius(blobShape, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(blobShape, 0, 0);
  lv_img_set_antialias(blobShape, false);  // keep pixel art crisp under zoom
  lv_obj_add_flag(blobShape, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(blobShape, LV_OBJ_FLAG_SCROLLABLE);
  // Zoom overflow handled via ext-draw-size (NOT OVERFLOW_VISIBLE, which
  // draws outside the box without telling the invalidation logic about it).
  lv_obj_add_event_cb(blobShape, blobExtDrawCB, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
  lv_obj_refresh_ext_draw_size(blobShape);

  eyeLeft  = lv_obj_create(blobShape);
  eyeRight = lv_obj_create(blobShape);
  // CRT-cyan (from Smol's own static/cape palette): the eyes render on the
  // black TV screen of the Smol sprite, so they glow instead of absorb.
  makeCircle(eyeLeft,  lv_color_hex(0x00CDF9));
  makeCircle(eyeRight, lv_color_hex(0x00CDF9));
  // One light glint pixel per eye (top-left): the detail that makes a flat
  // rectangle read as a drawn pixel eye — here it doubles as CRT shine.
  // Children clip to the eye, so blinks/squints swallow it naturally.
  for (lv_obj_t* eye : { eyeLeft, eyeRight }) {
    lv_obj_t* glint = lv_obj_create(eye);
    lv_obj_set_size(glint, 3, 3);
    lv_obj_set_style_radius(glint, 0, 0);
    lv_obj_set_style_bg_color(glint, lv_color_hex(0xD8F4FF), 0);
    lv_obj_set_style_bg_opa(glint, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(glint, 0, 0);
    lv_obj_set_style_pad_all(glint, 0, 0);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(glint, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  stageLabel = lv_label_create(petScreen);
  lv_obj_align(stageLabel, LV_ALIGN_CENTER, 0, 60);
  lv_obj_set_style_text_color(stageLabel, lv_color_white(), 0);

  xpLabel = lv_label_create(petScreen);
  lv_obj_align(xpLabel, LV_ALIGN_CENTER, 0, 90);
  lv_obj_set_style_text_color(xpLabel, lv_color_hex(0x4A6080), 0);

  xpBar = lv_bar_create(petScreen);
  lv_obj_set_size(xpBar, 140, 6);
  lv_obj_align(xpBar, LV_ALIGN_CENTER, 0, 110);
  lv_obj_set_style_bg_color(xpBar, lv_color_hex(0x0E0E1C), 0);
  lv_obj_set_style_bg_opa(xpBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(xpBar, lv_color_hex(0x50A8E8), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(xpBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(xpBar, 5, 0);
  lv_obj_set_style_radius(xpBar, 5, LV_PART_INDICATOR);

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

  // --- floating HP bar (game-style, hovers over the sprite and follows it):
  //     missed habits chip it at the daily reset; 0 = Koko dies. Created
  //     after blobShape so it draws on top; a small timer re-pins it above
  //     the sprite as the roam/hop/squash animations move him around ---
  healthLabel = lv_label_create(petScreen);
  lv_obj_set_width(healthLabel, 130);  // fits the "Koko  HP 100" nameplate
  lv_obj_set_style_text_align(healthLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(healthLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(healthLabel, lv_color_hex(0x3EE8A0), 0);
  lv_label_set_text(healthLabel, "Koko  HP 100");

  healthBar = lv_bar_create(petScreen);
  lv_obj_set_size(healthBar, 60, 6);
  lv_bar_set_range(healthBar, 0, 100);
  // Red track = the game convention for lost HP: the depleted part of the
  // bar reads as damage at a glance (a near-black track just looked like a
  // shorter bar).
  lv_obj_set_style_bg_color(healthBar, lv_color_hex(0x8A1622), 0);
  lv_obj_set_style_bg_opa(healthBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(healthBar, lv_color_hex(0x3EE8A0), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(healthBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(healthBar, 5, 0);
  lv_obj_set_style_radius(healthBar, 5, LV_PART_INDICATOR);
  positionHealthBar();
  lv_timer_create(healthFollowCB, 80, this);

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
      LV_SYMBOL_UP " settings   " LV_SYMBOL_DOWN " focus");
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

  // Clock — very top of the round glass, between the clipped corners
  // (~180 px of visible width up there; "23:59" is well under that).
  clockLabel = lv_label_create(petScreen);
  lv_obj_align(clockLabel, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_set_style_text_color(clockLabel, lv_color_hex(0x8890A8), 0);
  lv_label_set_text(clockLabel, "");
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
  lv_label_set_text(exitHint, "swipe " LV_SYMBOL_RIGHT " for Koko");
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
  lv_label_set_text(exitHint, LV_SYMBOL_LEFT " Koko    " LV_SYMBOL_RIGHT " goals");
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
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetZoom);
  lv_img_set_zoom(blobShape, depthZoom);  // snap back to resting depth before re-puffing
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetZoom);
  lv_anim_set_values(&a, depthZoom, depthZoom * 112 / 100);  // ~112% puff at any depth
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

  lv_img_set_src(blobShape, stageSprite(stage));
  lv_obj_set_size(blobShape, sz, sz);  // sprites are authored at exactly stageSize

  // No glow on the pixel-art sprites: the shadow follows the square widget
  // box (round-rect), which clashes with the sprite silhouette.
  lv_obj_set_style_shadow_opa(blobShape, LV_OPA_TRANSP, 0);

  // Eye layout: the eyes live on Smol's TV screen. gen_sprites.py pins the
  // screen center at (sz/2, sz*2/5), i.e. -sz/10 from widget center — these
  // offsets/sizes keep every expression (surprise 1.5x + drift) inside the
  // screen at all three scales; the tightest fit is the blob stage.
  eyeOffX  = sz / 20;
  eyeOffY  = -(sz / 10);
  eyeBaseW = sz / 16;
  g_eyeGrid = (sz >= 245) ? 6 : (sz >= 205) ? 5 : 4;  // Smol art scale (egg lands on 4)

  applyMoodExpression();
  startBlobAnimations();

  // Label: stage name + level
  int level = pet->getLevel();
  lv_label_set_text_fmt(stageLabel, "%s  LV.%d", pet->getStageName(), level);

  // XP progress within current level
  int xpIn   = pet->xpIntoCurrentLevel();
  int xpNeed = xpIn + pet->xpToNextLevel();
  lv_label_set_text_fmt(xpLabel, "%d / %d xp", xpIn, xpNeed);
  // Range tracks the level's cost (grows 50/level), so the bar always reads
  // as fraction-to-next-level.
  lv_bar_set_range(xpBar, 0, xpNeed);
  lv_bar_set_value(xpBar, xpIn, LV_ANIM_ON);

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

  // Health strip (top center) — same traffic-light thresholds as hunger.
  {
    int hp = pet->getHealth();
    uint32_t col = hp >= 70 ? 0x3EE8A0 : hp >= 40 ? 0xD4A030 : 0xFF4040;
    lv_bar_set_value(healthBar, hp, LV_ANIM_ON);
    lv_label_set_text_fmt(healthLabel, "Koko  HP %d", hp);
    lv_obj_set_style_text_color(healthLabel, lv_color_hex(col), 0);
    lv_obj_set_style_bg_color(healthBar, lv_color_hex(col), LV_PART_INDICATOR);
  }
}

/* ---- animations -------------------------------------------------- */

void PetUI::startBlobAnimations() {
  int stage = pet->getStage();

  // Cancel everything and kill the old roam timer
  lv_anim_del(blobShape, NULL);
  if (roamTimer) { lv_timer_del(roamTimer); roamTimer = nullptr; }

  // (Glow pulse removed with the sprite conversion — see refreshPetScreen.)

  // Kick off roaming — first move is immediate, then on timer
  // Period scales with mood: mood=100 → 1× base, mood=0 → 5× base
  roamToRandom();
  int mood0 = pet->getMood();
  uint32_t initPeriod = (uint32_t)(ROAM_PERIOD_MS[stage] * (25 + (100 - mood0)) / 25);
  roamTimer = lv_timer_create(roamTimerCB, initPeriod, this);
}

// Game-style floating HP bar: centered above the sprite's *drawn* top edge.
// g_depthZoom scales the rendered image around the widget center, so the
// visual top is centerY - drawn/2; the bar width tracks apparent size so it
// shrinks when Koko walks "away". Squash/stretch wobbles it a little — fine,
// it reads as alive. Called from a small timer (~12 Hz), cheap enough that
// gating beyond the active-screen check isn't worth it.
void PetUI::positionHealthBar() {
  if (lv_scr_act() != petScreen) return;
  int w  = lv_obj_get_width(blobShape);
  int h  = lv_obj_get_height(blobShape);
  int cx = lv_obj_get_x(blobShape) + w / 2;
  int cy = lv_obj_get_y(blobShape) + h / 2;
  int drawn = h * g_depthZoom / 256;
  int barW  = (w * g_depthZoom / 256) * 3 / 4;
  if (barW < 36) barW = 36;
  lv_obj_set_size(healthBar, barW, 6);
  lv_obj_set_pos(healthBar, cx - barW / 2, cy - drawn / 2 - 14);
  lv_obj_set_pos(healthLabel, cx - 65, cy - drawn / 2 - 32);
}

void PetUI::healthFollowCB(lv_timer_t* t) {
  ((PetUI*)t->user_data)->positionHealthBar();
}

void PetUI::roamToRandom() {
  int stage = pet->getStage();
  int sz    = stageSize(stage);
  lv_coord_t pW = lv_obj_get_width(petScreen);
  lv_coord_t pH = lv_obj_get_height(petScreen);

  // Cancel squash/stretch and settle at the current depth before roaming
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetX);
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetZoom);
  lv_anim_del(this,      (lv_anim_exec_xcb_t)animSetDepth);
  lv_img_set_zoom(blobShape, depthZoom);
  g_depthZoom = depthZoom;
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

  // Depth: each roam also picks how near/far the pet ends up (image zoom,
  // 66%..129%). Coming in close = walking toward the viewer.
  int targetDepth = 170 + rand() % 160;

  // A glide that clearly shrinks the depth is walking AWAY: show the back
  // view and hide the eyes (no face back there). The threshold keeps small
  // depth wobbles from flashing the back for a barely-receding glide.
  // The egg has no back view — it keeps its face (and eyes) at any depth.
  walkAway = stage != STAGE_EGG && targetDepth + 25 < depthZoom;
  if (walkAway) {
    lv_obj_add_flag(eyeLeft,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(eyeRight, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(eyeLeft,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(eyeRight, LV_OBJ_FLAG_HIDDEN);
  }

  // Eye drift: shift eyes toward the direction of movement — unless the
  // pet is coming up close, in which case it looks straight at the viewer.
  int dx = targetX - curX;
  int dy = targetY - curY;
  int absDx = dx < 0 ? -dx : dx;
  int absDy = dy < 0 ? -dy : dy;
  int dist  = absDx + absDy;
  if (targetDepth >= 280) {
    eyeDriftX = eyeDriftY = 0;   // looking at you
  } else if (dist > 4) {
    // Drift scales with stage: ±7 px would walk the eyes off the blob
    // stage's 28 px-wide TV screen.
    int driftMax = LV_MAX(2, stageSize(stage) / 40);
    eyeDriftX = (lv_coord_t)(dx * driftMax / dist);
    eyeDriftY = (lv_coord_t)(dy * driftMax / dist);
  } else {
    eyeDriftX = eyeDriftY = 0;
  }
  applyMoodExpression();

  // Depth glide: zoom eases from the current depth to the new one over the
  // whole travel (replaces the old in-flight swell — the depth change IS
  // the size dynamic now). animSetDepth also rescales the eyes each frame.
  // (NEVER resize an lv_img widget: v8 tiles the bitmap to fill a larger
  // box, drawing a second copy of the sprite.)
  int travelMs = ROAM_TRAVEL_MS[stage];
  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, this);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetDepth);
  lv_anim_set_values(&a, depthZoom, targetDepth);
  lv_anim_set_time(&a, travelMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
  depthZoom = targetDepth;  // resting depth once the glide lands

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

  // Walk cycle while traveling: flip between the two walk poses. Killed on
  // arrival (startIdleWobble restores the base frame). A pure-vertical
  // glide keeps the previous direction rather than snapping the sway.
  if (dx != 0) walkMirrored = dx > 0;
  if (walkAnimTimer) { lv_timer_del(walkAnimTimer); walkAnimTimer = nullptr; }
  walkFrameB = false;
  walkAnimTimer = lv_timer_create(walkFrameCB, 160, this);
  // Turn around NOW, not at the first 160ms frame flip — an eyeless front
  // view (or a back view with eyes) must never be shown.
  lv_img_set_src(blobShape, stageWalkSprite(stage, walkFrameB, walkMirrored, walkAway));
}

void PetUI::animSetDepth(void* petui, int32_t v) {
  PetUI* self = (PetUI*)petui;
  g_depthZoom = (int)v;
  lv_img_set_zoom(self->blobShape, (uint16_t)v);
  self->applyMoodExpression();   // keep the eyes glued to the scaling face
}

void PetUI::walkFrameCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  int stage = self->pet->getStage();
  self->walkFrameB = !self->walkFrameB;
  lv_img_set_src(self->blobShape,
                 stageWalkSprite(stage, self->walkFrameB, self->walkMirrored, self->walkAway));
}

void PetUI::startIdleWobble() {
  int stage = pet->getStage();
  int sz    = stageSize(stage);

  // Landed: stop the walk cycle on the base frame. Arrival always faces
  // the viewer again, so the back view ends and the eyes come back.
  if (walkAnimTimer) { lv_timer_del(walkAnimTimer); walkAnimTimer = nullptr; }
  walkFrameB = false;
  walkAway = false;
  lv_obj_clear_flag(eyeLeft,  LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(eyeRight, LV_OBJ_FLAG_HIDDEN);
  lv_img_set_src(blobShape, stageSprite(stage));

  // Settle at the roam's resting depth in case an anim didn't finish
  lv_anim_del(blobShape, (lv_anim_exec_xcb_t)animSetZoom);
  lv_anim_del(this,      (lv_anim_exec_xcb_t)animSetDepth);
  lv_img_set_zoom(blobShape, depthZoom);
  g_depthZoom = depthZoom;
  lv_obj_set_size(blobShape, sz, sz);
  lv_obj_set_pos(blobShape, blobBaseX, blobBaseY);

  // Center eyes (drift reset)
  eyeDriftX = eyeDriftY = 0;
  applyMoodExpression();

  // Landing bounce: quick zoom dip then spring back. (The old W≠H squash
  // isn't possible on an image — v8 zoom is uniform — so this reads as a
  // little compression instead; the center pivot keeps it in place.)
  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, blobShape);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetZoom);
  lv_anim_set_values(&a, depthZoom, depthZoom * 84 / 100);  // ~85% dip at any depth
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

void PetUI::showTrophyPill() {
  // Same construction as showLevelUpPill, gold trophy edition.
  lv_obj_t* pill = lv_obj_create(lv_layer_top());
  lv_obj_set_size(pill, 190, 44);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x1A1200), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pill, 22, 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(0xFFD060), 0);
  lv_obj_set_style_border_width(pill, 2, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_coord_t px = 233 - 95;
  lv_coord_t py = 150;
  lv_obj_set_pos(pill, px, py);

  lv_obj_t* lbl = lv_label_create(pill);
  lv_label_set_text(lbl, LV_SYMBOL_OK " NEW TROPHY!");
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFD060), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_center(lbl);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, pill);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetY);
  lv_anim_set_values(&a, py, py - 70);
  lv_anim_set_time(&a, 1800);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, xpPopupDeleteCB);
  lv_anim_set_user_data(&a, pill);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, pill);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)animSetOpa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, 900);
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
      self->showSettingsScreen();
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
// Focus is deliberately absent: it has a dedicated swipe on the pet screen
// (down); the menu is for apps that don't. (The old tap-counting workout app
// was removed July 2026 — the IMU-counted back/pull-up apps replaced it.)
static const AppEntry APP_ENTRIES[] = {
  { LV_SYMBOL_GPS,       "walk",     0x50A8E8 },
  { LV_SYMBOL_REFRESH,   "back",     0xFFA050 },
  { LV_SYMBOL_DOWN,      "push-ups", 0xF06090 },
  { LV_SYMBOL_UP,        "pull-ups", 0x60D080 },
  { LV_SYMBOL_DOWNLOAD,  "squats",   0xE87858 },
  { LV_SYMBOL_MINUS,     "hang",     0xA8E050 },
  { LV_SYMBOL_BARS,      "plank",    0xE8A050 },
  { LV_SYMBOL_EYE_CLOSE, "sleep",    0xA080FF },
  { LV_SYMBOL_HOME,      "clean room", 0xE8B040 },
  { LV_SYMBOL_BELL,      "sit",      0x40C8D8 },
  { LV_SYMBOL_TINT,      "meditate", 0x60C8A8 },
  { LV_SYMBOL_OK,        "trophies", 0xFFD060 },
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

  // Up to 4 apps: one big rounded button per app, stacked around center.
  // More than 4: a two-column grid — a single column would need sliver-thin
  // buttons to fit the round glass (44 px at 7 apps), while 160x68 grid
  // cells stay comfortably finger-sized. An odd last entry gets a centered
  // full-width row. Grid extents (4 rows = 296 px tall, 328 px wide) clear
  // the 466 px circle: at the top row's edge (y=85) the glass is 360 wide.
  const bool grid = APP_COUNT > 4;
  const int  rows = (APP_COUNT + 1) / 2;
  // 5 rows only fit the round glass with slightly shorter cells: 5×58+4×6 =
  // 314 px tall keeps the top row's button corners inside the 233 px radius.
  // 6 rows shrink again to 48: 6×48+5×6 = 318 px tall, and the top row's
  // outer corners sit at ~228 px from center — just inside the glass.
  const int  bw = 160, bh = (rows > 5) ? 48 : (rows > 4) ? 58 : 68,
             gx = 8, gy = (rows > 4) ? 6 : 8;
  const int  btnH = 72, gap = 20, pitch = btnH + gap;  // single-column sizes
  for (int i = 0; i < APP_COUNT; i++) {
    lv_obj_t* btn = lv_btn_create(appsScreen);
    if (grid) {
      bool lastOdd = (i == APP_COUNT - 1) && (APP_COUNT % 2 == 1);
      int row = i / 2, col = i % 2;
      lv_obj_set_size(btn, lastOdd ? 2 * bw + gx : bw, bh);
      lv_obj_align(btn, LV_ALIGN_CENTER,
                   lastOdd ? 0 : (col ? (bw + gx) / 2 : -(bw + gx) / 2),
                   row * (bh + gy) - ((rows - 1) * (bh + gy)) / 2);
    } else {
      lv_obj_set_size(btn, 280, btnH);
      lv_obj_align(btn, LV_ALIGN_CENTER, 0,
                   i * pitch - ((APP_COUNT - 1) * pitch) / 2);
    }
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
      self->showWalkScreen();
      break;
    case 1:
      self->showBackScreen();
      break;
    case 2:
      self->showPushScreen();
      break;
    case 3:
      self->showPullupScreen();
      break;
    case 4:
      self->showSquatScreen();
      break;
    case 5:
      self->showHangScreen();
      break;
    case 6:
      self->showPlankScreen();
      break;
    case 7:
      self->showSleepScreen();
      break;
    case 8:
      self->showCleanScreen();
      break;
    case 9:
      self->showSitScreen();
      break;
    case 10:
      self->showMedScreen();
      break;
    case 11:
      self->showTrophyScreen();
      break;
    default:
      break;
  }
}

/* ---- push-up screen -------------------------------------------------- */

void PetUI::setWorkoutReward(lv_obj_t* lbl, int xp, int bonusReps) {
  int d = pet->getSettings().difficulty;
  if (d < 0) d = 0;
  if (d > 2) d = 2;
  if (bonusReps > 0)
    lv_label_set_text_fmt(lbl, "gives: +%d xp, snack +%d food +%d mood\n%d+ reps: double snack",
                          xp, DIFF_MEAL_HUNGER[d], DIFF_MEAL_MOOD[d], bonusReps);
  else
    lv_label_set_text_fmt(lbl, "gives: +%d xp, snack +%d food +%d mood",
                          xp, DIFF_MEAL_HUNGER[d], DIFF_MEAL_MOOD[d]);
}

void PetUI::buildPushScreen() {
  pushScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(pushScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(pushScreen, LV_OBJ_FLAG_SCROLLABLE);
  // The whole screen is the rep target: a nose is not a precise stylus.
  lv_obj_add_flag(pushScreen, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* title = lv_label_create(pushScreen);
  lv_label_set_text(title, "PUSH-UPS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF06090), 0);

  pushRewardLabel = lv_label_create(pushScreen);
  lv_obj_set_style_text_font(pushRewardLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(pushRewardLabel, lv_color_hex(0x6A2A40), 0);
  lv_obj_set_style_text_align(pushRewardLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(pushRewardLabel, LV_ALIGN_TOP_MID, 0, 56);

  pushRepLabel = lv_label_create(pushScreen);
  lv_label_set_text(pushRepLabel, "0");
  lv_obj_set_style_text_font(pushRepLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(pushRepLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(pushRepLabel, LV_ALIGN_CENTER, 0, -40);

  pushHintLabel = lv_label_create(pushScreen);
  lv_label_set_text(pushHintLabel, "put it under you, press START");
  lv_obj_set_style_text_color(pushHintLabel, lv_color_hex(0x8A3A55), 0);
  lv_obj_set_style_text_align(pushHintLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(pushHintLabel, LV_ALIGN_CENTER, 0, 8);

  // START only — the button hides during the session. Finishing is the
  // physical BOOT button (the sketch calls finishPushSession): mid-push-up
  // an on-screen DONE is both a mis-tap hazard for the nose counter and
  // hard to hit deliberately.
  pushStartBtn = lv_btn_create(pushScreen);
  lv_obj_set_size(pushStartBtn, 240, 56);
  lv_obj_align(pushStartBtn, LV_ALIGN_CENTER, 0, 78);
  lv_obj_set_style_bg_color(pushStartBtn, lv_color_hex(0x2A0A18), 0);
  lv_obj_set_style_bg_opa(pushStartBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pushStartBtn, 16, 0);
  lv_obj_set_style_border_width(pushStartBtn, 2, 0);
  lv_obj_set_style_border_color(pushStartBtn, lv_color_hex(0xF06090), 0);
  pushBtnLabel = lv_label_create(pushStartBtn);
  lv_label_set_text(pushBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(pushBtnLabel, lv_color_hex(0xF06090), 0);
  lv_obj_center(pushBtnLabel);
  lv_obj_add_event_cb(pushStartBtn, pushBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(pushScreen);
  lv_label_set_text(hint, "BOOT button = exit");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  // Screen-level tap = one rep (the button consumes its own clicks and
  // doesn't bubble, so START/DONE presses never count as reps).
  // Deliberately NO gesture handler: sloppy workout taps read as swipes
  // and were exiting mid-set — BOOT is the only way off this screen, and
  // a stray swipe just lands in the rep counter.
  lv_obj_add_event_cb(pushScreen, pushTapCB, LV_EVENT_CLICKED, this);
}

void PetUI::showPushScreen() {
  pushReps = 0;
  pushRunning = false;
  setWorkoutReward(pushRewardLabel, PUSH_XP, PUSH_BONUS_REPS);
  lv_label_set_text(pushRepLabel, "0");
  lv_label_set_text(pushHintLabel, "put it under you, press START");
  lv_obj_clear_flag(pushStartBtn, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load_anim(pushScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::pushTapCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (!self->pushRunning) return;
  // Swipe-release clicks and touch bounce are not push-ups: same gesture
  // guard as everywhere, plus a 400 ms tap debounce (even fast push-ups
  // leave more than that between nose touches).
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  if (lv_tick_get() - self->lastPushTapMs < 400) return;
  self->lastPushTapMs = lv_tick_get();

  self->pushReps++;
  lv_label_set_text_fmt(self->pushRepLabel, "%d", self->pushReps);
  if (self->soundCB) self->soundCB(SOUND_REP_BLIP);  // audible confirm per nose-tap
  if (self->pushReps == PUSH_TARGET_REPS)
    lv_label_set_text(self->pushHintLabel, "target hit!\nkeep going, or BOOT = done");
}

void PetUI::pushBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  if (self->pushRunning) return;  // unreachable (button hidden), belt & braces

  self->pushRunning = true;
  self->pushReps = 0;
  self->lastPushTapMs = lv_tick_get();  // arm debounce so this press can't count
  lv_label_set_text(self->pushRepLabel, "0");
  lv_label_set_text_fmt(self->pushHintLabel,
      "boop with your nose! %d = snack\nBOOT button = done", PUSH_TARGET_REPS);
  lv_obj_add_flag(self->pushStartBtn, LV_OBJ_FLAG_HIDDEN);
}

// Session end, driven by the physical BOOT button (sketch calls this when
// isPushRunning()). Awards if the target was reached, else back to armed.
void PetUI::finishPushSession() {
  if (!pushRunning) return;
  pushRunning = false;
  lv_obj_clear_flag(pushStartBtn, LV_OBJ_FLAG_HIDDEN);
  if (pushReps >= PUSH_TARGET_REPS) {
    pet->feedWorkout();  // meal size scales with dashboard difficulty
    if (pushReps >= PUSH_BONUS_REPS) pet->feedWorkout();  // 15+ = double snack
    pet->addXP(PUSH_XP);
    refreshPetScreen();
    if (pushReps >= PUSH_BONUS_REPS)
      lv_label_set_text_fmt(pushHintLabel, "beast! DOUBLE snack  +%d xp", PUSH_XP);
    else
      lv_label_set_text_fmt(pushHintLabel, "fed Koko  +%d xp", PUSH_XP);
    if (soundCB) soundCB(SOUND_HABIT_DONE);
    if (pushDoneCB) pushDoneCB();
    lv_timer_t* t = lv_timer_create(pushDoneTimerCB, 1200, this);
    lv_timer_set_repeat_count(t, 1);
  } else {
    lv_label_set_text(pushHintLabel, "put it under you, press START");
  }
}

void PetUI::pushDoneTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (lv_scr_act() == self->pushScreen) self->showPetScreen();
}

/* ---- squat screen ----------------------------------------------------- */
// Same contract as push-ups: every screen tap is one rep, the physical BOOT
// button ends the session (the sketch calls finishSquatSession while
// isSquatRunning()). Set the device somewhere tappable — a chair or stool in
// front of you — and tap at the bottom of each rep.

void PetUI::buildSquatScreen() {
  squatScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(squatScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(squatScreen, LV_OBJ_FLAG_SCROLLABLE);
  // The whole screen is the rep target, same as the push-up nose counter.
  lv_obj_add_flag(squatScreen, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* title = lv_label_create(squatScreen);
  lv_label_set_text(title, "SQUATS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE87858), 0);

  squatRewardLabel = lv_label_create(squatScreen);
  lv_obj_set_style_text_font(squatRewardLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(squatRewardLabel, lv_color_hex(0x6A3428), 0);
  lv_obj_set_style_text_align(squatRewardLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(squatRewardLabel, LV_ALIGN_TOP_MID, 0, 56);

  squatRepLabel = lv_label_create(squatScreen);
  lv_label_set_text(squatRepLabel, "0");
  lv_obj_set_style_text_font(squatRepLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(squatRepLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(squatRepLabel, LV_ALIGN_CENTER, 0, -40);

  squatHintLabel = lv_label_create(squatScreen);
  lv_label_set_text(squatHintLabel, "set it where you can tap, press START");
  lv_obj_set_style_text_color(squatHintLabel, lv_color_hex(0x8A4A34), 0);
  lv_obj_set_style_text_align(squatHintLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(squatHintLabel, LV_ALIGN_CENTER, 0, 8);

  // START only — hidden during the session; the physical BOOT button ends
  // it (finishSquatSession), same reasoning as push-ups: mid-rep an
  // on-screen DONE is a mis-tap hazard for the rep counter.
  squatStartBtn = lv_btn_create(squatScreen);
  lv_obj_set_size(squatStartBtn, 240, 56);
  lv_obj_align(squatStartBtn, LV_ALIGN_CENTER, 0, 78);
  lv_obj_set_style_bg_color(squatStartBtn, lv_color_hex(0x2A140C), 0);
  lv_obj_set_style_bg_opa(squatStartBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(squatStartBtn, 16, 0);
  lv_obj_set_style_border_width(squatStartBtn, 2, 0);
  lv_obj_set_style_border_color(squatStartBtn, lv_color_hex(0xE87858), 0);
  lv_obj_t* btnLbl = lv_label_create(squatStartBtn);
  lv_label_set_text(btnLbl, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(btnLbl, lv_color_hex(0xE87858), 0);
  lv_obj_center(btnLbl);
  lv_obj_add_event_cb(squatStartBtn, squatBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(squatScreen);
  lv_label_set_text(hint, "BOOT button = exit");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  // Screen-level tap = one rep (START consumes its own clicks and doesn't
  // bubble, so pressing it never counts as a rep).
  // Deliberately NO gesture handler, same as push-ups: BOOT is the only
  // way off this screen; a stray swipe just lands in the rep counter.
  lv_obj_add_event_cb(squatScreen, squatTapCB, LV_EVENT_CLICKED, this);
}

void PetUI::showSquatScreen() {
  squatReps = 0;
  squatRunning = false;
  setWorkoutReward(squatRewardLabel, SQUAT_XP, SQUAT_BONUS_REPS);
  lv_label_set_text(squatRepLabel, "0");
  lv_label_set_text(squatHintLabel, "set it where you can tap, press START");
  lv_obj_clear_flag(squatStartBtn, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load_anim(squatScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::squatTapCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (!self->squatRunning) return;
  // Swipe-release clicks and touch bounce are not squats: same gesture
  // guard as everywhere, plus a tap debounce (even fast squats leave more
  // than 400 ms between bottoms).
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  if (lv_tick_get() - self->lastSquatTapMs < 400) return;
  self->lastSquatTapMs = lv_tick_get();

  self->squatReps++;
  lv_label_set_text_fmt(self->squatRepLabel, "%d", self->squatReps);
  if (self->soundCB) self->soundCB(SOUND_REP_BLIP);  // audible confirm per rep
  if (self->squatReps == SQUAT_TARGET_REPS)
    lv_label_set_text(self->squatHintLabel, "target hit!\nkeep going, or BOOT = done");
}

void PetUI::squatBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  if (self->squatRunning) return;  // unreachable (button hidden), belt & braces

  self->squatRunning = true;
  self->squatReps = 0;
  self->lastSquatTapMs = lv_tick_get();  // arm debounce so this press can't count
  lv_label_set_text(self->squatRepLabel, "0");
  lv_label_set_text_fmt(self->squatHintLabel,
      "tap every rep! %d = snack\nBOOT button = done", SQUAT_TARGET_REPS);
  lv_obj_add_flag(self->squatStartBtn, LV_OBJ_FLAG_HIDDEN);
}

// Session end, driven by the physical BOOT button (sketch calls this when
// isSquatRunning()). Awards if the target was reached, else back to armed.
void PetUI::finishSquatSession() {
  if (!squatRunning) return;
  squatRunning = false;
  lv_obj_clear_flag(squatStartBtn, LV_OBJ_FLAG_HIDDEN);
  if (squatReps >= SQUAT_TARGET_REPS) {
    pet->feedWorkout();  // meal size scales with dashboard difficulty
    if (squatReps >= SQUAT_BONUS_REPS) pet->feedWorkout();  // 25+ = double snack
    pet->addXP(SQUAT_XP);
    refreshPetScreen();
    if (squatReps >= SQUAT_BONUS_REPS)
      lv_label_set_text_fmt(squatHintLabel, "beast! DOUBLE snack  +%d xp", SQUAT_XP);
    else
      lv_label_set_text_fmt(squatHintLabel, "fed Koko  +%d xp", SQUAT_XP);
    if (soundCB) soundCB(SOUND_HABIT_DONE);
    lv_timer_t* t = lv_timer_create(squatDoneTimerCB, 1200, this);
    lv_timer_set_repeat_count(t, 1);
  } else {
    lv_label_set_text(squatHintLabel, "set it where you can tap, press START");
  }
}

void PetUI::squatDoneTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (lv_scr_act() == self->squatScreen) self->showPetScreen();
}

/* ---- back-workout screen -------------------------------------------- */

void PetUI::buildBackScreen() {
  backScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(backScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(backScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(backScreen);
  lv_label_set_text(title, "BACK WORKOUT");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFA050), 0);

  backRewardLabel = lv_label_create(backScreen);
  lv_obj_set_style_text_font(backRewardLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(backRewardLabel, lv_color_hex(0x6A4520), 0);
  lv_obj_set_style_text_align(backRewardLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(backRewardLabel, LV_ALIGN_TOP_MID, 0, 58);

  backRepLabel = lv_label_create(backScreen);
  lv_label_set_text(backRepLabel, "0");
  lv_obj_set_style_text_font(backRepLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(backRepLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(backRepLabel, LV_ALIGN_CENTER, 0, -40);

  backHintLabel = lv_label_create(backScreen);
  lv_label_set_text(backHintLabel, "hold Koko, press START");
  lv_obj_set_style_text_color(backHintLabel, lv_color_hex(0x8A5A28), 0);
  lv_obj_align(backHintLabel, LV_ALIGN_CENTER, 0, 8);

  lv_obj_t* btn = lv_btn_create(backScreen);
  lv_obj_set_size(btn, 240, 56);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 78);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A1C08), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0xFFA050), 0);
  backBtnLabel = lv_label_create(btn);
  lv_label_set_text(backBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(backBtnLabel, lv_color_hex(0xFFA050), 0);
  lv_obj_center(backBtnLabel);
  lv_obj_add_event_cb(btn, backBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(backScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(backScreen, backGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::showBackScreen() {
  backReps = 0;
  backRunning = false;
  setWorkoutReward(backRewardLabel, BACK_XP);
  lv_label_set_text(backRepLabel, "0");
  lv_label_set_text(backHintLabel, "hold Koko, press START");
  lv_label_set_text(backBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_scr_load_anim(backScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::addBackRep() {
  if (!backRunning) return;
  backReps++;
  lv_label_set_text_fmt(backRepLabel, "%d", backReps);
  if (soundCB) soundCB(SOUND_REP_BLIP);  // audible confirm mid-swing
  if (backReps == BACK_TARGET_REPS)
    lv_label_set_text(backHintLabel, "target hit! keep going or DONE");
}

void PetUI::backBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;

  if (!self->backRunning) {
    self->backRunning = true;
    self->backReps = 0;
    lv_label_set_text(self->backRepLabel, "0");
    lv_label_set_text_fmt(self->backHintLabel, "swing wide! %d reps = snack", BACK_TARGET_REPS);
    lv_label_set_text_fmt(self->backBtnLabel, LV_SYMBOL_OK "  DONE (need %d)", BACK_TARGET_REPS);
    return;
  }

  self->backRunning = false;
  if (self->backReps >= BACK_TARGET_REPS) {
    self->pet->feedWorkout();  // meal size scales with dashboard difficulty
    self->pet->addXP(BACK_XP);
    self->refreshPetScreen();
    lv_label_set_text_fmt(self->backHintLabel, "fed Koko  +%d xp", BACK_XP);
    if (self->soundCB) self->soundCB(SOUND_HABIT_DONE);
    if (self->backDoneCB) self->backDoneCB();
    // Let the reward text land, then back to the pet to see the effect.
    lv_timer_t* t = lv_timer_create(backDoneTimerCB, 1200, self);
    lv_timer_set_repeat_count(t, 1);
  } else {
    lv_label_set_text(self->backHintLabel, "hold Koko, press START");
    lv_label_set_text(self->backBtnLabel, LV_SYMBOL_PLAY "  START");
  }
}

void PetUI::backDoneTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (lv_scr_act() == self->backScreen) self->showPetScreen();
}

void PetUI::backGestureCB(lv_event_t* e) {
  // Any swipe cancels the session (no award) and returns to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->backRunning = false;
  self->showPetScreen();
}

/* ---- pull-up screen -------------------------------------------------- */

void PetUI::buildPullupScreen() {
  pullupScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(pullupScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(pullupScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(pullupScreen);
  lv_label_set_text(title, "PULL-UPS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0x60D080), 0);

  pullupRewardLabel = lv_label_create(pullupScreen);
  lv_obj_set_style_text_font(pullupRewardLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(pullupRewardLabel, lv_color_hex(0x2A6A40), 0);
  lv_obj_set_style_text_align(pullupRewardLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(pullupRewardLabel, LV_ALIGN_TOP_MID, 0, 58);

  pullupRepLabel = lv_label_create(pullupScreen);
  lv_label_set_text(pullupRepLabel, "0");
  lv_obj_set_style_text_font(pullupRepLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(pullupRepLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(pullupRepLabel, LV_ALIGN_CENTER, 0, -40);

  pullupHintLabel = lv_label_create(pullupScreen);
  lv_label_set_text(pullupHintLabel, "Koko in pocket, press START");
  lv_obj_set_style_text_color(pullupHintLabel, lv_color_hex(0x2A6A40), 0);
  lv_obj_align(pullupHintLabel, LV_ALIGN_CENTER, 0, 8);

  lv_obj_t* btn = lv_btn_create(pullupScreen);
  lv_obj_set_size(btn, 240, 56);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 78);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x08201C), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x60D080), 0);
  pullupBtnLabel = lv_label_create(btn);
  lv_label_set_text(pullupBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(pullupBtnLabel, lv_color_hex(0x60D080), 0);
  lv_obj_center(pullupBtnLabel);
  lv_obj_add_event_cb(btn, pullupBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(pullupScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(pullupScreen, pullupGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::showPullupScreen() {
  pullupReps = 0;
  pullupRunning = false;
  setWorkoutReward(pullupRewardLabel, PULLUP_XP);
  lv_label_set_text(pullupRepLabel, "0");
  lv_label_set_text(pullupHintLabel, "Koko in pocket, press START");
  lv_label_set_text(pullupBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_scr_load_anim(pullupScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::addPullupRep() {
  if (!pullupRunning) return;
  pullupReps++;
  lv_label_set_text_fmt(pullupRepLabel, "%d", pullupReps);
  if (soundCB) soundCB(SOUND_REP_BLIP);  // audible confirm — screen's in a pocket
  if (pullupReps == PULLUP_TARGET_REPS)
    lv_label_set_text(pullupHintLabel, "target hit! keep going or DONE");
}

void PetUI::pullupBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;

  if (!self->pullupRunning) {
    self->pullupRunning = true;
    self->pullupReps = 0;
    lv_label_set_text(self->pullupRepLabel, "0");
    lv_label_set_text_fmt(self->pullupHintLabel, "hang & pull! %d reps = snack", PULLUP_TARGET_REPS);
    lv_label_set_text_fmt(self->pullupBtnLabel, LV_SYMBOL_OK "  DONE (need %d)", PULLUP_TARGET_REPS);
    return;
  }

  self->pullupRunning = false;
  if (self->pullupReps >= PULLUP_TARGET_REPS) {
    self->pet->feedWorkout();  // meal size scales with dashboard difficulty
    self->pet->addXP(PULLUP_XP);
    self->refreshPetScreen();
    lv_label_set_text_fmt(self->pullupHintLabel, "fed Koko  +%d xp", PULLUP_XP);
    if (self->soundCB) self->soundCB(SOUND_HABIT_DONE);
    if (self->pullupDoneCB) self->pullupDoneCB();
    // Let the reward text land, then back to the pet to see the effect.
    lv_timer_t* t = lv_timer_create(pullupDoneTimerCB, 1200, self);
    lv_timer_set_repeat_count(t, 1);
  } else {
    lv_label_set_text(self->pullupHintLabel, "Koko in pocket, press START");
    lv_label_set_text(self->pullupBtnLabel, LV_SYMBOL_PLAY "  START");
  }
}

void PetUI::pullupDoneTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (lv_scr_act() == self->pullupScreen) self->showPetScreen();
}

void PetUI::pullupGestureCB(lv_event_t* e) {
  // Any swipe cancels the session (no award) and returns to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->pullupRunning = false;
  self->showPetScreen();
}

/* ---- meditation screen ------------------------------------------------ */
// Pick 10 or 20 minutes, press START, sit still. The screen darkens ~10 s
// in (a lit panel is the opposite of meditation; BOOT peeks at the time)
// and relights itself at the end with a gentle chime. Finishing heals HP:
// 10 min = +10, 20 min = +40 — the long sit pays disproportionately, per
// the user's spec. Stopping early or swiping away heals nothing.

static const uint8_t  MED_CHOICES[2] = { 10, 20 };   // minutes
static const int      MED_HEAL[2]    = { 10, 40 };   // hp
static const uint32_t MED_DARK_DELAY_MS = 10000;

void PetUI::buildMedScreen() {
  medScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(medScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(medScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(medScreen);
  lv_label_set_text(title, "MEDITATE");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0x60C8A8), 0);

  medArc = lv_arc_create(medScreen);
  lv_obj_set_size(medArc, 280, 280);
  lv_obj_align(medArc, LV_ALIGN_CENTER, 0, -30);
  lv_arc_set_rotation(medArc, 270);
  lv_arc_set_bg_angles(medArc, 0, 359);
  lv_arc_set_range(medArc, 0, 1000);
  lv_arc_set_value(medArc, 1000);
  lv_obj_set_style_arc_color(medArc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
  lv_obj_set_style_arc_width(medArc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_color(medArc, lv_color_hex(0x60C8A8), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(medArc, 14, LV_PART_INDICATOR);
  lv_obj_set_style_opa(medArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(medArc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(medArc, LV_OBJ_FLAG_CLICKABLE);

  medTimeLabel = lv_label_create(medScreen);
  lv_label_set_text(medTimeLabel, "10:00");
  lv_obj_set_style_text_font(medTimeLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(medTimeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(medTimeLabel, LV_ALIGN_CENTER, 0, -46);

  medHintLabel = lv_label_create(medScreen);
  lv_label_set_text(medHintLabel, "sit still, breathe\nscreen sleeps once you start");
  lv_obj_set_style_text_color(medHintLabel, lv_color_hex(0x2A6A58), 0);
  lv_obj_set_style_text_align(medHintLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(medHintLabel, LV_ALIGN_CENTER, 0, -4);

  lv_obj_t* btn = lv_btn_create(medScreen);
  lv_obj_set_size(btn, 190, 48);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 56);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x082A20), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 14, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x60C8A8), 0);
  medBtnLabel = lv_label_create(btn);
  lv_label_set_text(medBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(medBtnLabel, lv_color_hex(0x60C8A8), 0);
  lv_obj_center(medBtnLabel);
  lv_obj_add_event_cb(btn, medBtnCB, LV_EVENT_CLICKED, this);

  // Duration choices double as the reward blurb.
  for (int i = 0; i < 2; i++) {
    lv_obj_t* cb = lv_btn_create(medScreen);
    lv_obj_set_size(cb, 140, 38);
    lv_obj_align(cb, LV_ALIGN_TOP_MID, (i * 152) - 76, 340);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x10101E), 0);
    lv_obj_set_style_radius(cb, 10, 0);
    lv_obj_set_style_border_width(cb, 2, 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0x1E4438), 0);
    lv_obj_t* lbl = lv_label_create(cb);
    lv_label_set_text_fmt(lbl, "%dm  +%d hp", MED_CHOICES[i], MED_HEAL[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x8AB0A0), 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(cb, (void*)(intptr_t)i);
    lv_obj_add_event_cb(cb, medChoiceCB, LV_EVENT_CLICKED, this);
    medChoiceBtns[i] = cb;
  }

  lv_obj_t* hint = lv_label_create(medScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(medScreen, medGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::medMarkChoice() {
  for (int i = 0; i < 2; i++) {
    bool sel = (MED_CHOICES[i] == medIntervalMin);
    lv_obj_set_style_border_color(medChoiceBtns[i],
      lv_color_hex(sel ? 0x60C8A8 : 0x1E4438), 0);
  }
}

void PetUI::medUpdateAux() {
  for (int i = 0; i < 2; i++) {
    if (medRunning) lv_obj_add_flag(medChoiceBtns[i], LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_clear_flag(medChoiceBtns[i], LV_OBJ_FLAG_HIDDEN);
  }
}

void PetUI::showMedScreen() {
  if (!medRunning) {
    lv_arc_set_value(medArc, 1000);
    lv_label_set_text_fmt(medTimeLabel, "%02d:00", medIntervalMin);
    lv_label_set_text(medHintLabel, "sit still, breathe\nscreen sleeps once you start");
    lv_label_set_text(medBtnLabel, LV_SYMBOL_PLAY "  START");
  }
  medMarkChoice();
  medUpdateAux();
  lv_scr_load_anim(medScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

// relight: end-of-session paths that may run with the panel dark MUST turn
// it back on — the dark state also suppresses touch (sketch), so leaving it
// dark after medRunning clears would strand the user on a black screen.
void PetUI::medStop(bool relight) {
  medRunning = false;
  if (medClockTimer) { lv_timer_del(medClockTimer); medClockTimer = nullptr; }
  if (relight && medDarkened && screenPowerCB) screenPowerCB(true);
  medDarkened = false;
  lv_arc_set_value(medArc, 1000);
  lv_label_set_text_fmt(medTimeLabel, "%02d:00", medIntervalMin);
  lv_label_set_text(medBtnLabel, LV_SYMBOL_PLAY "  START");
  medUpdateAux();
}

void PetUI::medBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;

  if (!self->medRunning) {
    self->medRunning  = true;
    self->medDarkened = false;
    self->medStartMs  = lv_tick_get();
    lv_label_set_text_fmt(self->medHintLabel,
        "breathe. +%d hp at the end\nBOOT peeks at the time",
        MED_HEAL[self->medIntervalMin == MED_CHOICES[1] ? 1 : 0]);
    lv_label_set_text(self->medBtnLabel, LV_SYMBOL_STOP "  STOP");
    self->medUpdateAux();
    if (!self->medClockTimer)
      self->medClockTimer = lv_timer_create(medClockCB, 500, self);
    return;
  }
  // STOP early — no heal.
  self->medStop(true);
  lv_label_set_text(self->medHintLabel, "sit still, breathe\nscreen sleeps once you start");
}

void PetUI::medChoiceCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (self->medRunning) return;
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  self->medIntervalMin = MED_CHOICES[idx];
  self->medMarkChoice();
  lv_label_set_text_fmt(self->medTimeLabel, "%02d:00", self->medIntervalMin);
}

void PetUI::medClockCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (!self->medRunning) return;

  uint32_t totalMs = (uint32_t)self->medIntervalMin * 60000UL;
  uint32_t elapsed = lv_tick_get() - self->medStartMs;

  if (!self->medDarkened && elapsed >= MED_DARK_DELAY_MS) {
    self->medDarkened = true;
    if (self->screenPowerCB) self->screenPowerCB(false);
  }

  if (elapsed >= totalMs) {
    int heal = MED_HEAL[self->medIntervalMin == MED_CHOICES[1] ? 1 : 0];
    self->medStop(true);  // relights before the chime — the reward should be seen
    self->pet->heal(heal);
    self->refreshPetScreen();
    lv_label_set_text_fmt(self->medHintLabel, "inner peace  +%d hp", heal);
    if (self->soundCB) self->soundCB(SOUND_HABIT_DONE);
    return;
  }

  uint32_t remaining = totalMs - elapsed;
  lv_arc_set_value(self->medArc, (int)(remaining * 1000 / totalMs));
  uint32_t secs = (remaining + 999) / 1000;
  lv_label_set_text_fmt(self->medTimeLabel, "%02u:%02u",
                        (unsigned)(secs / 60), (unsigned)(secs % 60));
}

void PetUI::medGestureCB(lv_event_t* e) {
  // Any swipe cancels the session (no heal) and returns to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->medStop(true);
  self->showPetScreen();
}

/* ---- clean room screen ------------------------------------------------ */
// 3-minute speed-clean sprint: hit START, put the pet down, tidy until the
// ring runs out. Finishing awards XP; STOP or swiping away forfeits. No food
// on purpose — meals stay earned by exercise.

static const uint32_t CLEAN_ROOM_MS = 3 * 60 * 1000;
static const int      CLEAN_XP      = 15;

void PetUI::buildCleanScreen() {
  cleanScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(cleanScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(cleanScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(cleanScreen);
  lv_label_set_text(title, "CLEAN ROOM");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE8B040), 0);

  // Countdown ring, same construction as the focus arc.
  cleanArc = lv_arc_create(cleanScreen);
  lv_obj_set_size(cleanArc, 300, 300);
  lv_obj_align(cleanArc, LV_ALIGN_CENTER, 0, -16);
  lv_arc_set_rotation(cleanArc, 270);
  lv_arc_set_bg_angles(cleanArc, 0, 359);
  lv_arc_set_range(cleanArc, 0, 1000);
  lv_arc_set_value(cleanArc, 1000);
  lv_obj_set_style_arc_color(cleanArc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
  lv_obj_set_style_arc_width(cleanArc, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_color(cleanArc, lv_color_hex(0xE8B040), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(cleanArc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_opa(cleanArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(cleanArc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(cleanArc, LV_OBJ_FLAG_CLICKABLE);

  cleanTimeLabel = lv_label_create(cleanScreen);
  lv_label_set_text(cleanTimeLabel, "03:00");
  lv_obj_set_style_text_font(cleanTimeLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(cleanTimeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(cleanTimeLabel, LV_ALIGN_CENTER, 0, -32);

  cleanHintLabel = lv_label_create(cleanScreen);
  lv_label_set_text(cleanHintLabel, "3 minutes. go go go!");
  lv_obj_set_style_text_color(cleanHintLabel, lv_color_hex(0x8A6A28), 0);
  lv_obj_align(cleanHintLabel, LV_ALIGN_CENTER, 0, 8);

  // START/STOP button sits inside the ring's hole, like the focus labels.
  lv_obj_t* btn = lv_btn_create(cleanScreen);
  lv_obj_set_size(btn, 200, 52);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 74);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x201808), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0xE8B040), 0);
  cleanBtnLabel = lv_label_create(btn);
  lv_label_set_text(cleanBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(cleanBtnLabel, lv_color_hex(0xE8B040), 0);
  lv_obj_center(cleanBtnLabel);
  lv_obj_add_event_cb(btn, cleanBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* reward = lv_label_create(cleanScreen);
  lv_label_set_text(reward, "gives: +15 xp");
  lv_obj_set_style_text_font(reward, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(reward, lv_color_hex(0x6A5520), 0);
  lv_obj_align(reward, LV_ALIGN_CENTER, 0, 112);

  lv_obj_t* hint = lv_label_create(cleanScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(cleanScreen, cleanGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::showCleanScreen() {
  cleanRunning = false;
  if (cleanClockTimer) { lv_timer_del(cleanClockTimer); cleanClockTimer = nullptr; }
  lv_arc_set_value(cleanArc, 1000);
  lv_label_set_text(cleanTimeLabel, "03:00");
  lv_label_set_text(cleanHintLabel, "3 minutes. go go go!");
  lv_label_set_text(cleanBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_scr_load_anim(cleanScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::cleanBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;

  if (!self->cleanRunning) {
    self->cleanRunning = true;
    self->cleanStartMs = lv_tick_get();
    lv_arc_set_value(self->cleanArc, 1000);
    lv_label_set_text(self->cleanTimeLabel, "03:00");
    lv_label_set_text(self->cleanHintLabel, "tidy! Koko is watching");
    lv_label_set_text(self->cleanBtnLabel, LV_SYMBOL_STOP "  STOP");
    if (!self->cleanClockTimer)
      self->cleanClockTimer = lv_timer_create(cleanClockCB, 500, self);
    return;
  }

  // STOP mid-sprint — forfeit, back to the armed state.
  self->cleanRunning = false;
  if (self->cleanClockTimer) { lv_timer_del(self->cleanClockTimer); self->cleanClockTimer = nullptr; }
  lv_arc_set_value(self->cleanArc, 1000);
  lv_label_set_text(self->cleanTimeLabel, "03:00");
  lv_label_set_text(self->cleanHintLabel, "3 minutes. go go go!");
  lv_label_set_text(self->cleanBtnLabel, LV_SYMBOL_PLAY "  START");
}

void PetUI::cleanClockCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (!self->cleanRunning) return;

  uint32_t elapsed = lv_tick_get() - self->cleanStartMs;
  if (elapsed >= CLEAN_ROOM_MS) {
    // Sprint done — celebrate and stay on screen (AGAIN re-arms via START).
    self->cleanRunning = false;
    lv_timer_del(t);
    self->cleanClockTimer = nullptr;
    lv_arc_set_value(self->cleanArc, 0);
    lv_label_set_text(self->cleanTimeLabel, "00:00");
    lv_label_set_text_fmt(self->cleanHintLabel, "room cleaned!  +%d xp", CLEAN_XP);
    lv_label_set_text(self->cleanBtnLabel, LV_SYMBOL_REFRESH "  AGAIN");

    int stageBefore = self->pet->getStage();
    int levelBefore = self->pet->getLevel();
    self->pet->addXP(CLEAN_XP);
    self->refreshPetScreen();
    lv_area_t area;
    lv_obj_get_coords(self->cleanArc, &area);
    self->showXpPopup(area, CLEAN_XP);
    if (self->pet->getStage() > stageBefore) self->showEvolutionBurst();
    else if (self->pet->getLevel() > levelBefore)
      self->showLevelUpPill(self->pet->getLevel());
    if (self->soundCB) self->soundCB(SOUND_HABIT_DONE);
    return;
  }

  uint32_t remaining = CLEAN_ROOM_MS - elapsed;
  lv_arc_set_value(self->cleanArc, (int)(remaining * 1000 / CLEAN_ROOM_MS));
  uint32_t secs = (remaining + 999) / 1000;  // ceil to avoid an early "00:00"
  lv_label_set_text_fmt(self->cleanTimeLabel, "%02u:%02u",
                        (unsigned)(secs / 60), (unsigned)(secs % 60));
}

void PetUI::cleanGestureCB(lv_event_t* e) {
  // Any swipe cancels the sprint (no award) and returns to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->cleanRunning = false;
  if (self->cleanClockTimer) { lv_timer_del(self->cleanClockTimer); self->cleanClockTimer = nullptr; }
  self->showPetScreen();
}

/* ---- hang screen ------------------------------------------------------ */
// Bar-hang progression: three isometric holds unlocked in order — bar hang,
// then scapula hang, then L-sit. Tap anywhere to start, tap again when you
// drop (mid-hang nobody has a finger free for a button). Holding to 1:00
// banks the base reward (HANG_XP + snack) no matter how the set ends;
// the full 2:00 auto-completes with DOUBLE everything and unlocks the next
// stage. Dropping under 1:00 forfeits. Unlocks reset at reboot — the ladder
// is re-earned each gym session.

static const char* HANG_NAMES[3] = { "hang", "scapula", "L-sit" };

void PetUI::buildHangScreen() {
  hangScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(hangScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(hangScreen, LV_OBJ_FLAG_SCROLLABLE);
  // The whole screen is the start/stop control (same idea as the push-up
  // rep target): the tap lands right before jumping up / after dropping.
  lv_obj_add_flag(hangScreen, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* title = lv_label_create(hangScreen);
  lv_label_set_text(title, "HANG");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0xA8E050), 0);

  // Stage pills: the unlock ladder. Locked ones stay tappable and explain
  // themselves via the hint — a disabled-dead button reads as broken.
  for (int i = 0; i < 3; i++) {
    lv_obj_t* btn = lv_btn_create(hangScreen);
    lv_obj_set_size(btn, 104, 38);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, (i - 1) * 112, 64);
    lv_obj_set_style_radius(btn, 19, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, hangStageBtnCB, LV_EVENT_CLICKED, this);
    hangStageBtns[i] = btn;
    hangStageLbls[i] = lbl;
  }

  // Fill-up ring: empty at 00:00, full at the 2:00 target (the inverse of
  // the countdown apps — a hold grows, a sprint drains).
  hangArc = lv_arc_create(hangScreen);
  lv_obj_set_size(hangArc, 250, 250);
  lv_obj_align(hangArc, LV_ALIGN_CENTER, 0, 30);
  lv_arc_set_rotation(hangArc, 270);
  lv_arc_set_bg_angles(hangArc, 0, 359);
  lv_arc_set_range(hangArc, 0, 1000);
  lv_arc_set_value(hangArc, 0);
  lv_obj_set_style_arc_color(hangArc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
  lv_obj_set_style_arc_width(hangArc, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_color(hangArc, lv_color_hex(0xA8E050), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(hangArc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_opa(hangArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(hangArc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(hangArc, LV_OBJ_FLAG_CLICKABLE);  // taps fall through to the screen

  hangTimeLabel = lv_label_create(hangScreen);
  lv_label_set_text(hangTimeLabel, "00:00");
  lv_obj_set_style_text_font(hangTimeLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(hangTimeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(hangTimeLabel, LV_ALIGN_CENTER, 0, 4);

  hangHintLabel = lv_label_create(hangScreen);
  lv_label_set_text(hangHintLabel, "");
  lv_obj_set_style_text_font(hangHintLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hangHintLabel, lv_color_hex(0x6A8A38), 0);
  lv_obj_set_style_text_align(hangHintLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hangHintLabel, LV_ALIGN_CENTER, 0, 46);

  hangRewardLabel = lv_label_create(hangScreen);
  lv_label_set_text(hangRewardLabel, "");
  lv_obj_set_style_text_font(hangRewardLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hangRewardLabel, lv_color_hex(0x556A28), 0);
  lv_obj_set_style_text_align(hangRewardLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hangRewardLabel, LV_ALIGN_CENTER, 0, 84);

  lv_obj_t* hint = lv_label_create(hangScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(hangScreen, hangTapCB, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(hangScreen, hangGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::hangRefreshStages() {
  for (int i = 0; i < 3; i++) {
    bool locked = (i > 0) && !hangPassed[i - 1];
    bool sel    = (i == hangStage);
    uint32_t col = locked ? 0x3A3A4A : 0xA8E050;
    lv_obj_set_style_border_color(hangStageBtns[i], lv_color_hex(col), 0);
    lv_obj_set_style_border_width(hangStageBtns[i], sel ? 3 : 1, 0);
    lv_obj_set_style_bg_color(hangStageBtns[i], lv_color_hex(sel ? 0x22300C : 0x10101E), 0);
    lv_obj_set_style_text_color(hangStageLbls[i], lv_color_hex(col), 0);
    if (locked)
      lv_label_set_text_fmt(hangStageLbls[i], LV_SYMBOL_CLOSE " %s", HANG_NAMES[i]);
    else if (hangPassed[i])
      lv_label_set_text_fmt(hangStageLbls[i], LV_SYMBOL_OK " %s", HANG_NAMES[i]);
    else
      lv_label_set_text(hangStageLbls[i], HANG_NAMES[i]);
  }
}

void PetUI::hangSelectStage(int s) {
  hangStage = s;
  lv_arc_set_value(hangArc, 0);
  lv_label_set_text(hangTimeLabel, "00:00");
  lv_label_set_text_fmt(hangHintLabel, "%s to 2:00 — tap to start", HANG_NAMES[s]);
  hangRefreshStages();
}

void PetUI::showHangScreen() {
  hangRunning = false;
  if (hangClockTimer) { lv_timer_del(hangClockTimer); hangClockTimer = nullptr; }
  // Custom two-tier reward line (setWorkoutReward only knows one tier);
  // snack numbers follow the dashboard difficulty like the other workouts.
  int d = pet->getSettings().difficulty;
  if (d < 0) d = 0;
  if (d > 2) d = 2;
  lv_label_set_text_fmt(hangRewardLabel,
                        "1:00 gives: +%d xp, snack +%d food +%d mood\n2:00: double it all + next unlock",
                        HANG_XP, DIFF_MEAL_HUNGER[d], DIFF_MEAL_MOOD[d]);
  // Open on the next rung of the ladder: the first stage not yet passed.
  int s = 0;
  while (s < 2 && hangPassed[s]) s++;
  hangSelectStage(s);
  lv_scr_load_anim(hangScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::hangTapCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  // Swipes emit a CLICKED on release; same guard as the other app screens.
  if (lv_tick_get() - self->lastGestureMs < 600) return;

  if (!self->hangRunning) {
    self->hangRunning = true;
    self->hangStartMs = lv_tick_get();
    lv_arc_set_value(self->hangArc, 0);
    lv_label_set_text(self->hangTimeLabel, "00:00");
    lv_label_set_text(self->hangHintLabel, "hold on! tap when you drop");
    if (!self->hangClockTimer)
      self->hangClockTimer = lv_timer_create(hangClockCB, 250, self);
    return;
  }

  // Dropped before 2:00. Past 1:00 the base reward is banked either way;
  // under 1:00 forfeits. The time stays up so you know your best.
  uint32_t held = lv_tick_get() - self->hangStartMs;
  self->hangRunning = false;
  if (self->hangClockTimer) { lv_timer_del(self->hangClockTimer); self->hangClockTimer = nullptr; }
  unsigned secs = (unsigned)(held / 1000);
  if (held >= HANG_PASS_MS) {
    self->hangAward(1);
    lv_label_set_text_fmt(self->hangHintLabel, "held %02u:%02u — snack earned! 2:00 doubles it",
                          secs / 60, secs % 60);
  } else {
    lv_label_set_text_fmt(self->hangHintLabel, "dropped at %02u:%02u — 1:00 earns the snack",
                          secs / 60, secs % 60);
  }
}

// Shared reward for both tiers: mult=1 for a 1:00 hold, mult=2 for the full
// 2:00. Two feedWorkout() calls (not one big meal) so the double snack
// tracks the difficulty table exactly like the push-up bonus does.
void PetUI::hangAward(int mult) {
  int stageBefore = pet->getStage();
  int levelBefore = pet->getLevel();
  pet->addXP(HANG_XP * mult);
  for (int i = 0; i < mult; i++) pet->feedWorkout();
  refreshPetScreen();
  lv_area_t area;
  lv_obj_get_coords(hangArc, &area);
  showXpPopup(area, HANG_XP * mult);
  if (pet->getStage() > stageBefore) showEvolutionBurst();
  else if (pet->getLevel() > levelBefore) showLevelUpPill(pet->getLevel());
  if (soundCB) soundCB(SOUND_HABIT_DONE);
}

void PetUI::hangStageBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  if (self->hangRunning) return;  // no switching mid-hold
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if (idx > 0 && !self->hangPassed[idx - 1]) {
    lv_label_set_text_fmt(self->hangHintLabel, "hold %s to 2:00 to unlock %s",
                          HANG_NAMES[idx - 1], HANG_NAMES[idx]);
    return;
  }
  self->hangSelectStage(idx);
}

void PetUI::hangClockCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (!self->hangRunning) return;

  uint32_t elapsed = lv_tick_get() - self->hangStartMs;
  if (elapsed < HANG_TARGET_MS) {
    lv_arc_set_value(self->hangArc, (int)(elapsed * 1000 / HANG_TARGET_MS));
    unsigned secs = (unsigned)(elapsed / 1000);
    lv_label_set_text_fmt(self->hangTimeLabel, "%02u:%02u", secs / 60, secs % 60);
    // Flip the hint once when the base reward is banked (strcmp guards the
    // 250 ms tick from re-setting — and invalidating — identical text).
    static const char* SECURED = "snack banked! 2:00 doubles it";
    if (elapsed >= HANG_PASS_MS &&
        strcmp(lv_label_get_text(self->hangHintLabel), SECURED) != 0)
      lv_label_set_text(self->hangHintLabel, SECURED);
    return;
  }

  // Full 2:00 held — double reward, then advance the ladder. Repeat
  // completions pay again (it's still exercise); the unlock only matters
  // the first time.
  self->hangRunning = false;
  lv_timer_del(t);
  self->hangClockTimer = nullptr;
  int done = self->hangStage;
  self->hangPassed[done] = true;
  self->hangAward(2);

  if (done < 2) {
    // Jump the selection to the freshly unlocked stage, armed and ready.
    self->hangSelectStage(done + 1);
    lv_label_set_text_fmt(self->hangHintLabel, "2:00 — double snack! next: %s",
                          HANG_NAMES[done + 1]);
  } else {
    lv_arc_set_value(self->hangArc, 1000);
    lv_label_set_text(self->hangTimeLabel, "02:00");
    lv_label_set_text(self->hangHintLabel, "full 2:00 set! Koko feasts double");
    self->hangRefreshStages();  // repaint the L-sit pill with its check
  }
}

void PetUI::hangGestureCB(lv_event_t* e) {
  // Any swipe closes; a running hold is forfeited (no partial credit).
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->hangRunning = false;
  if (self->hangClockTimer) { lv_timer_del(self->hangClockTimer); self->hangClockTimer = nullptr; }
  self->showPetScreen();
}

/* ---- plank screen ----------------------------------------------------- */
// Single isometric hold, the hang app's little sibling: tap anywhere to
// start, tap again when you collapse. 1:00 banks the base reward, the full
// 2:00 auto-completes with double. Unlike hang there's no ladder — instead
// every real hold (>= PLANK_MIN_RECORD_MS) is remembered: last and best
// ride PlankDoneCB to NVS and the dashboard's plank analytics.

void PetUI::buildPlankScreen() {
  plankScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(plankScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(plankScreen, LV_OBJ_FLAG_SCROLLABLE);
  // Whole screen = start/stop control, same as hang: mid-plank the tap
  // lands with a forearm, not a fingertip.
  lv_obj_add_flag(plankScreen, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* title = lv_label_create(plankScreen);
  lv_label_set_text(title, "PLANK");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE8A050), 0);

  plankStatsLabel = lv_label_create(plankScreen);
  lv_label_set_text(plankStatsLabel, "no holds yet");
  lv_obj_set_style_text_font(plankStatsLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(plankStatsLabel, lv_color_hex(0x8A6A38), 0);
  lv_obj_align(plankStatsLabel, LV_ALIGN_TOP_MID, 0, 68);

  // Fill-up ring toward the 2:00 target, same inverse-countdown as hang.
  plankArc = lv_arc_create(plankScreen);
  lv_obj_set_size(plankArc, 250, 250);
  lv_obj_align(plankArc, LV_ALIGN_CENTER, 0, 24);
  lv_arc_set_rotation(plankArc, 270);
  lv_arc_set_bg_angles(plankArc, 0, 359);
  lv_arc_set_range(plankArc, 0, 1000);
  lv_arc_set_value(plankArc, 0);
  lv_obj_set_style_arc_color(plankArc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
  lv_obj_set_style_arc_width(plankArc, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_color(plankArc, lv_color_hex(0xE8A050), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(plankArc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_opa(plankArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(plankArc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(plankArc, LV_OBJ_FLAG_CLICKABLE);  // taps fall through to the screen

  plankTimeLabel = lv_label_create(plankScreen);
  lv_label_set_text(plankTimeLabel, "00:00");
  lv_obj_set_style_text_font(plankTimeLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(plankTimeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(plankTimeLabel, LV_ALIGN_CENTER, 0, -2);

  plankHintLabel = lv_label_create(plankScreen);
  lv_label_set_text(plankHintLabel, "");
  lv_obj_set_style_text_font(plankHintLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(plankHintLabel, lv_color_hex(0xA87838), 0);
  lv_obj_set_style_text_align(plankHintLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(plankHintLabel, LV_ALIGN_CENTER, 0, 40);

  plankRewardLabel = lv_label_create(plankScreen);
  lv_label_set_text(plankRewardLabel, "");
  lv_obj_set_style_text_font(plankRewardLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(plankRewardLabel, lv_color_hex(0x6A5228), 0);
  lv_obj_set_style_text_align(plankRewardLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(plankRewardLabel, LV_ALIGN_CENTER, 0, 80);

  lv_obj_t* hint = lv_label_create(plankScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(plankScreen, plankTapCB, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(plankScreen, plankGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::plankRefreshStats() {
  if (plankLastMs == 0 && plankBestMs == 0) {
    lv_label_set_text(plankStatsLabel, "no holds yet");
    return;
  }
  unsigned l = (unsigned)(plankLastMs / 1000), b = (unsigned)(plankBestMs / 1000);
  lv_label_set_text_fmt(plankStatsLabel, "last %02u:%02u   best %02u:%02u",
                        l / 60, l % 60, b / 60, b % 60);
}

void PetUI::setPlankStats(uint32_t lastMs, uint32_t bestMs) {
  plankLastMs = lastMs;
  plankBestMs = bestMs;
  plankRefreshStats();
}

void PetUI::showPlankScreen() {
  plankRunning = false;
  if (plankClockTimer) { lv_timer_del(plankClockTimer); plankClockTimer = nullptr; }
  // Same two-tier reward line as hang; snack numbers follow the difficulty.
  int d = pet->getSettings().difficulty;
  if (d < 0) d = 0;
  if (d > 2) d = 2;
  lv_label_set_text_fmt(plankRewardLabel,
                        "1:00 gives: +%d xp, snack +%d food +%d mood\n2:00: double it all",
                        PLANK_XP, DIFF_MEAL_HUNGER[d], DIFF_MEAL_MOOD[d]);
  lv_arc_set_value(plankArc, 0);
  lv_label_set_text(plankTimeLabel, "00:00");
  lv_label_set_text(plankHintLabel, "tap to start the hold");
  plankRefreshStats();
  lv_scr_load_anim(plankScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

// Session bookkeeping shared by the two ways a hold ends (tap-stop and the
// 2:00 auto-complete). Mis-taps under PLANK_MIN_RECORD_MS don't count.
void PetUI::plankRecord(uint32_t heldMs) {
  if (heldMs < PLANK_MIN_RECORD_MS) return;
  plankLastMs = heldMs;
  if (heldMs > plankBestMs) plankBestMs = heldMs;
  plankRefreshStats();
  if (plankDoneCB) plankDoneCB(heldMs);
}

void PetUI::plankAward(int mult) {
  int stageBefore = pet->getStage();
  int levelBefore = pet->getLevel();
  pet->addXP(PLANK_XP * mult);
  for (int i = 0; i < mult; i++) pet->feedWorkout();
  refreshPetScreen();
  lv_area_t area;
  lv_obj_get_coords(plankArc, &area);
  showXpPopup(area, PLANK_XP * mult);
  if (pet->getStage() > stageBefore) showEvolutionBurst();
  else if (pet->getLevel() > levelBefore) showLevelUpPill(pet->getLevel());
  if (soundCB) soundCB(SOUND_HABIT_DONE);
}

void PetUI::plankTapCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  // Swipes emit a CLICKED on release; same guard as the other app screens.
  if (lv_tick_get() - self->lastGestureMs < 600) return;

  if (!self->plankRunning) {
    self->plankRunning = true;
    self->plankStartMs = lv_tick_get();
    lv_arc_set_value(self->plankArc, 0);
    lv_label_set_text(self->plankTimeLabel, "00:00");
    lv_label_set_text(self->plankHintLabel, "hold steady! tap when you drop");
    if (!self->plankClockTimer)
      self->plankClockTimer = lv_timer_create(plankClockCB, 250, self);
    return;
  }

  // Dropped before 2:00. Past 1:00 the base reward is banked; under 1:00
  // no snack, but the hold still goes on record (that's the point of the
  // last/best line — progress you can see next session).
  uint32_t held = lv_tick_get() - self->plankStartMs;
  self->plankRunning = false;
  if (self->plankClockTimer) { lv_timer_del(self->plankClockTimer); self->plankClockTimer = nullptr; }
  self->plankRecord(held);
  unsigned secs = (unsigned)(held / 1000);
  if (held >= PLANK_PASS_MS) {
    self->plankAward(1);
    lv_label_set_text_fmt(self->plankHintLabel, "held %02u:%02u — snack earned! 2:00 doubles it",
                          secs / 60, secs % 60);
  } else {
    lv_label_set_text_fmt(self->plankHintLabel, "dropped at %02u:%02u — 1:00 earns the snack",
                          secs / 60, secs % 60);
  }
}

void PetUI::plankClockCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (!self->plankRunning) return;

  uint32_t elapsed = lv_tick_get() - self->plankStartMs;
  if (elapsed < PLANK_TARGET_MS) {
    lv_arc_set_value(self->plankArc, (int)(elapsed * 1000 / PLANK_TARGET_MS));
    unsigned secs = (unsigned)(elapsed / 1000);
    lv_label_set_text_fmt(self->plankTimeLabel, "%02u:%02u", secs / 60, secs % 60);
    // One-shot hint flip at 1:00, strcmp-guarded against the 250 ms tick.
    static const char* SECURED = "snack banked! 2:00 doubles it";
    if (elapsed >= PLANK_PASS_MS &&
        strcmp(lv_label_get_text(self->plankHintLabel), SECURED) != 0)
      lv_label_set_text(self->plankHintLabel, SECURED);
    return;
  }

  // Full 2:00 held — auto-complete with the double reward.
  self->plankRunning = false;
  lv_timer_del(t);
  self->plankClockTimer = nullptr;
  self->plankRecord(PLANK_TARGET_MS);
  self->plankAward(2);
  lv_arc_set_value(self->plankArc, 1000);
  lv_label_set_text(self->plankTimeLabel, "02:00");
  lv_label_set_text(self->plankHintLabel, "full 2:00 plank! Koko feasts double");
}

void PetUI::plankGestureCB(lv_event_t* e) {
  // Any swipe closes; a running hold is forfeited (no partial credit),
  // matching hang — an abandoned screen shouldn't keep a timer alive.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->plankRunning = false;
  if (self->plankClockTimer) { lv_timer_del(self->plankClockTimer); self->plankClockTimer = nullptr; }
  self->showPetScreen();
}

/* ---- sitting / move-reminder screen ----------------------------------- */
// "I'm sitting": pick an interval, press START, put the device on the desk.
// When the interval runs out the screen turns alarm-red and an insistent
// chime repeats every minute until you tap that you moved — that re-arms
// the app (START begins the next round when you actually sit back down).
// Swipe closes and stops the reminders. The sketch inhibits auto-sleep
// while a session runs; a deep-sleeping device can't nag.

static const uint8_t  SIT_CHOICES[4] = { 5, 10, 15, 20 };  // minutes (20 max — short cycles)
static const uint32_t SIT_CHIME_GAP_MS = 60000;

void PetUI::buildSitScreen() {
  sitScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(sitScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(sitScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(sitScreen, LV_OBJ_FLAG_CLICKABLE);  // tap anywhere = "I moved" ack

  lv_obj_t* title = lv_label_create(sitScreen);
  lv_label_set_text(title, "SIT");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0x40C8D8), 0);

  sitArc = lv_arc_create(sitScreen);
  lv_obj_set_size(sitArc, 280, 280);
  lv_obj_align(sitArc, LV_ALIGN_CENTER, 0, -30);
  lv_arc_set_rotation(sitArc, 270);
  lv_arc_set_bg_angles(sitArc, 0, 359);
  lv_arc_set_range(sitArc, 0, 1000);
  lv_arc_set_value(sitArc, 1000);
  lv_obj_set_style_arc_color(sitArc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
  lv_obj_set_style_arc_width(sitArc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_color(sitArc, lv_color_hex(0x40C8D8), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(sitArc, 14, LV_PART_INDICATOR);
  lv_obj_set_style_opa(sitArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sitArc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(sitArc, LV_OBJ_FLAG_CLICKABLE);

  sitTimeLabel = lv_label_create(sitScreen);
  lv_label_set_text(sitTimeLabel, "20:00");
  lv_obj_set_style_text_font(sitTimeLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(sitTimeLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(sitTimeLabel, LV_ALIGN_CENTER, 0, -46);

  sitHintLabel = lv_label_create(sitScreen);
  lv_label_set_text(sitHintLabel, "sitting? I'll nag you to stand\ngives: nothing - saves your back");
  lv_obj_set_style_text_color(sitHintLabel, lv_color_hex(0x1E6A74), 0);
  lv_obj_set_style_text_align(sitHintLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sitHintLabel, LV_ALIGN_CENTER, 0, -4);

  lv_obj_t* btn = lv_btn_create(sitScreen);
  lv_obj_set_size(btn, 190, 48);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 56);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x08222A), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 14, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x40C8D8), 0);
  sitBtnLabel = lv_label_create(btn);
  lv_label_set_text(sitBtnLabel, LV_SYMBOL_PLAY "  START");
  lv_obj_set_style_text_color(sitBtnLabel, lv_color_hex(0x40C8D8), 0);
  lv_obj_center(sitBtnLabel);
  lv_obj_add_event_cb(btn, sitBtnCB, LV_EVENT_CLICKED, this);

  // Interval row (only reacts while idle)
  for (int i = 0; i < 4; i++) {
    lv_obj_t* cb = lv_btn_create(sitScreen);
    lv_obj_set_size(cb, 64, 36);
    lv_obj_align(cb, LV_ALIGN_TOP_MID, (i * 72) - 108, 340);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x10101E), 0);
    lv_obj_set_style_radius(cb, 10, 0);
    lv_obj_set_style_border_width(cb, 2, 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0x1E3A44), 0);
    lv_obj_t* lbl = lv_label_create(cb);
    lv_label_set_text_fmt(lbl, "%dm", SIT_CHOICES[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x8AA8B0), 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(cb, (void*)(intptr_t)i);
    lv_obj_add_event_cb(cb, sitChoiceCB, LV_EVENT_CLICKED, this);
    sitChoiceBtns[i] = cb;
  }

  // "screen off" — takes the choice row's spot while a session runs. Saves
  // battery over a long sit; BOOT wakes the display (the sketch owns the
  // panel via screenPowerCB) and the alert always wakes it by itself.
  sitScreenOffBtn = lv_btn_create(sitScreen);
  lv_obj_set_size(sitScreenOffBtn, 200, 36);
  lv_obj_align(sitScreenOffBtn, LV_ALIGN_TOP_MID, 0, 340);
  lv_obj_set_style_bg_color(sitScreenOffBtn, lv_color_hex(0x10101E), 0);
  lv_obj_set_style_radius(sitScreenOffBtn, 10, 0);
  lv_obj_set_style_border_width(sitScreenOffBtn, 2, 0);
  lv_obj_set_style_border_color(sitScreenOffBtn, lv_color_hex(0x1E3A44), 0);
  {
    lv_obj_t* lbl = lv_label_create(sitScreenOffBtn);
    lv_label_set_text(lbl, LV_SYMBOL_EYE_CLOSE "  screen off (BOOT wakes)");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x8AA8B0), 0);
    lv_obj_center(lbl);
  }
  lv_obj_add_flag(sitScreenOffBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(sitScreenOffBtn, [](lv_event_t* e) {
    PetUI* self = (PetUI*)lv_event_get_user_data(e);
    if (self->screenPowerCB) self->screenPowerCB(false);
  }, LV_EVENT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(sitScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(sitScreen, sitTapCB, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(sitScreen, sitGestureCB, LV_EVENT_GESTURE, this);
}

// While idle the interval choices show; while running they swap for the
// screen-off button (interval is locked anyway). Neither shows during the
// alert — the whole screen is the "I moved" button then.
void PetUI::sitUpdateAux() {
  for (int i = 0; i < 4; i++) {
    if (sitRunning) lv_obj_add_flag(sitChoiceBtns[i], LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_clear_flag(sitChoiceBtns[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (sitRunning && !sitAlerting) lv_obj_clear_flag(sitScreenOffBtn, LV_OBJ_FLAG_HIDDEN);
  else                            lv_obj_add_flag(sitScreenOffBtn, LV_OBJ_FLAG_HIDDEN);
}

void PetUI::sitMarkChoice() {
  for (int i = 0; i < 4; i++) {
    bool sel = (SIT_CHOICES[i] == sitIntervalMin);
    lv_obj_set_style_border_color(sitChoiceBtns[i],
      lv_color_hex(sel ? 0x40C8D8 : 0x1E3A44), 0);
  }
}

void PetUI::showSitScreen() {
  // Re-entering mid-session keeps the countdown (it's the whole point);
  // fresh entry shows the armed state.
  if (!sitRunning) {
    lv_arc_set_value(sitArc, 1000);
    lv_label_set_text_fmt(sitTimeLabel, "%02d:00", sitIntervalMin);
    lv_label_set_text(sitHintLabel, "sitting? I'll nag you to stand\ngives: nothing - saves your back");
    lv_label_set_text(sitBtnLabel, LV_SYMBOL_PLAY "  START");
  }
  sitMarkChoice();
  sitUpdateAux();
  lv_scr_load_anim(sitScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::sitStop() {
  sitRunning = false;
  sitAlerting = false;
  if (sitClockTimer) { lv_timer_del(sitClockTimer); sitClockTimer = nullptr; }
  lv_obj_set_style_bg_color(sitScreen, lv_color_hex(0x000000), 0);
  lv_arc_set_value(sitArc, 1000);
  lv_label_set_text_fmt(sitTimeLabel, "%02d:00", sitIntervalMin);
  lv_label_set_text(sitHintLabel, "sitting? I'll nag you to stand\ngives: nothing - saves your back");
  lv_label_set_text(sitBtnLabel, LV_SYMBOL_PLAY "  START");
  sitUpdateAux();
}

void PetUI::sitEnterAlert() {
  sitAlerting = true;
  sitLastChimeMs = lv_tick_get();
  if (screenPowerCB) screenPowerCB(true);  // a dark nag is no nag
  sitUpdateAux();
  lv_obj_set_style_bg_color(sitScreen, lv_color_hex(0x38100A), 0);
  lv_arc_set_value(sitArc, 0);
  lv_label_set_text(sitTimeLabel, "MOVE!");
  lv_label_set_text(sitHintLabel, "stand up, stretch, walk a bit\ntap anywhere when you did");
  lv_label_set_text(sitBtnLabel, LV_SYMBOL_OK "  I MOVED");
  if (soundCB) soundCB(SOUND_MOVE_ALERT);
}

void PetUI::sitAckMoved() {
  // Back to armed — the next round starts only when the user presses START
  // again (auto-restart nagged people who had actually left the desk).
  sitStop();
  lv_label_set_text(sitHintLabel, "nice! press START when you sit again");
}

void PetUI::sitBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;

  if (!self->sitRunning) {
    self->sitRunning  = true;
    self->sitAlerting = false;
    self->sitStartMs  = lv_tick_get();
    lv_label_set_text_fmt(self->sitHintLabel, "reminder in %d min", self->sitIntervalMin);
    lv_label_set_text(self->sitBtnLabel, LV_SYMBOL_STOP "  STOP");
    self->sitUpdateAux();
    if (!self->sitClockTimer)
      self->sitClockTimer = lv_timer_create(sitClockCB, 500, self);
    return;
  }
  if (self->sitAlerting) { self->sitAckMoved(); return; }
  self->sitStop();
}

void PetUI::sitChoiceCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (self->sitRunning) return;  // interval locked during a session
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  self->sitIntervalMin = SIT_CHOICES[idx];
  self->sitMarkChoice();
  lv_label_set_text_fmt(self->sitTimeLabel, "%02d:00", self->sitIntervalMin);
}

void PetUI::sitTapCB(lv_event_t* e) {
  // During the alert the whole screen is the "I moved" button — precision
  // is not a thing right after being startled off a chair.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (!self->sitAlerting) return;
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  self->sitAckMoved();
}

void PetUI::sitClockCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (!self->sitRunning) return;

  if (self->sitAlerting) {
    // Nag again every minute until acknowledged.
    if (lv_tick_get() - self->sitLastChimeMs >= SIT_CHIME_GAP_MS) {
      self->sitLastChimeMs = lv_tick_get();
      if (self->soundCB) self->soundCB(SOUND_MOVE_ALERT);
    }
    return;
  }

  uint32_t totalMs = (uint32_t)self->sitIntervalMin * 60000UL;
  uint32_t elapsed = lv_tick_get() - self->sitStartMs;
  if (elapsed >= totalMs) {
    self->sitEnterAlert();
    return;
  }
  uint32_t remaining = totalMs - elapsed;
  lv_arc_set_value(self->sitArc, (int)(remaining * 1000 / totalMs));
  uint32_t secs = (remaining + 999) / 1000;
  lv_label_set_text_fmt(self->sitTimeLabel, "%02u:%02u",
                        (unsigned)(secs / 60), (unsigned)(secs % 60));
}

void PetUI::sitGestureCB(lv_event_t* e) {
  // Any swipe stops the reminders and returns to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->sitStop();
  self->showPetScreen();
}

/* ---- settings screen ------------------------------------------------- */

// Pet-screen background palette (index = DeviceSettings.petBg). Dark tints
// only: the AMOLED's per-pixel power and the sprite's contrast both want a
// near-black stage.
static const uint32_t PET_BG_COLORS[5] =
  { 0x000000, 0x0A1428, 0x081810, 0x160826, 0x1C1206 };
static const char*    PET_BG_NAMES[5] =
  { "black", "night", "forest", "plum", "ember" };
static const uint8_t  SLEEP_CHOICES[4] = { 1, 2, 5, 10 };
static const char*    THEME_NAMES[3]   = { "classic", "arcade", "soft" };

// Small section label helper, shared by all settings rows.
static lv_obj_t* settingsLabel(lv_obj_t* parent, const char* text, int y) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_text_color(l, lv_color_hex(0x4A4A66), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  return l;
}

void PetUI::buildSettingsScreen() {
  settingsScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(settingsScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(settingsScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(settingsScreen);
  lv_label_set_text(title, "SETTINGS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_text_color(title, lv_color_hex(0x8A9AB8), 0);

  // volume
  settingsLabel(settingsScreen, "volume", 62);
  setVolSlider = lv_slider_create(settingsScreen);
  lv_obj_set_size(setVolSlider, 240, 14);
  lv_obj_align(setVolSlider, LV_ALIGN_TOP_MID, 0, 86);
  lv_slider_set_range(setVolSlider, 0, 100);
  lv_obj_set_style_bg_color(setVolSlider, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
  lv_obj_set_style_bg_color(setVolSlider, lv_color_hex(0x50A8E8), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(setVolSlider, lv_color_hex(0xC8D0E0), LV_PART_KNOB);
  lv_obj_add_event_cb(setVolSlider, setVolSliderCB, LV_EVENT_RELEASED, this);

  // sound theme
  settingsLabel(settingsScreen, "sounds", 112);
  for (int i = 0; i < 3; i++) {
    lv_obj_t* btn = lv_btn_create(settingsScreen);
    lv_obj_set_size(btn, 92, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, (i - 1) * 100, 134);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, THEME_NAMES[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, setThemeBtnCB, LV_EVENT_CLICKED, this);
    setThemeBtns[i] = btn;
  }

  // brightness
  settingsLabel(settingsScreen, "brightness", 180);
  setBriSlider = lv_slider_create(settingsScreen);
  lv_obj_set_size(setBriSlider, 240, 14);
  lv_obj_align(setBriSlider, LV_ALIGN_TOP_MID, 0, 204);
  lv_slider_set_range(setBriSlider, 20, 255);   // 20 floor: never invisible
  lv_obj_set_style_bg_color(setBriSlider, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
  lv_obj_set_style_bg_color(setBriSlider, lv_color_hex(0xFFD060), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(setBriSlider, lv_color_hex(0xC8D0E0), LV_PART_KNOB);
  lv_obj_add_event_cb(setBriSlider, setBriSliderCB, LV_EVENT_VALUE_CHANGED, this);

  // pet background
  settingsLabel(settingsScreen, "pet background", 230);
  for (int i = 0; i < 5; i++) {
    lv_obj_t* btn = lv_btn_create(settingsScreen);
    lv_obj_set_size(btn, 40, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, (i - 2) * 48, 252);
    lv_obj_set_style_radius(btn, 10, 0);
    // swatch shows the actual color; black gets a faint fill to stay visible
    lv_obj_set_style_bg_color(btn, lv_color_hex(i ? PET_BG_COLORS[i] : 0x14141E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, setBgBtnCB, LV_EVENT_CLICKED, this);
    setBgBtns[i] = btn;
  }

  // auto-sleep timeout
  settingsLabel(settingsScreen, "auto-sleep", 294);
  for (int i = 0; i < 4; i++) {
    lv_obj_t* btn = lv_btn_create(settingsScreen);
    lv_obj_set_size(btn, 56, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, (i * 64) - 96, 316);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%dm", SLEEP_CHOICES[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, setSleepBtnCB, LV_EVENT_CLICKED, this);
    setSleepBtns[i] = btn;
  }

  // battery toggles, side by side: step counter (background IMU polling) and
  // lift-to-wake (IMU wake-on-motion during auto-sleep). Either off = less
  // drain; tap-to-wake keeps working no matter what.
  lv_obj_t* stepsLbl = settingsLabel(settingsScreen, "step counter", 358);
  lv_obj_align(stepsLbl, LV_ALIGN_TOP_MID, -78, 358);
  setStepsSwitch = lv_switch_create(settingsScreen);
  lv_obj_set_size(setStepsSwitch, 60, 28);
  lv_obj_align(setStepsSwitch, LV_ALIGN_TOP_MID, -78, 378);
  lv_obj_set_style_bg_color(setStepsSwitch, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
  lv_obj_set_style_bg_color(setStepsSwitch, lv_color_hex(0xA8E050),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(setStepsSwitch, lv_color_hex(0xC8D0E0), LV_PART_KNOB);
  lv_obj_add_event_cb(setStepsSwitch, [](lv_event_t* e) {
    PetUI* self = (PetUI*)lv_event_get_user_data(e);
    lv_obj_t* sw = lv_event_get_target(e);
    if (lv_tick_get() - self->lastGestureMs < 600) {
      // tail of the opening swipe, not a real tap — put the state back
      if (self->devSettings.stepsOn) lv_obj_add_state(sw, LV_STATE_CHECKED);
      else lv_obj_clear_state(sw, LV_STATE_CHECKED);
      return;
    }
    self->devSettings.stepsOn = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if (self->settingsCB) self->settingsCB(self->devSettings);
  }, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t* liftLbl = settingsLabel(settingsScreen, "lift to wake", 358);
  lv_obj_align(liftLbl, LV_ALIGN_TOP_MID, 78, 358);
  setLiftSwitch = lv_switch_create(settingsScreen);
  lv_obj_set_size(setLiftSwitch, 60, 28);
  lv_obj_align(setLiftSwitch, LV_ALIGN_TOP_MID, 78, 378);
  lv_obj_set_style_bg_color(setLiftSwitch, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
  lv_obj_set_style_bg_color(setLiftSwitch, lv_color_hex(0xFFD060),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(setLiftSwitch, lv_color_hex(0xC8D0E0), LV_PART_KNOB);
  lv_obj_add_event_cb(setLiftSwitch, [](lv_event_t* e) {
    PetUI* self = (PetUI*)lv_event_get_user_data(e);
    lv_obj_t* sw = lv_event_get_target(e);
    if (lv_tick_get() - self->lastGestureMs < 600) {
      if (self->devSettings.liftWake) lv_obj_add_state(sw, LV_STATE_CHECKED);
      else lv_obj_clear_state(sw, LV_STATE_CHECKED);
      return;
    }
    self->devSettings.liftWake = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if (self->settingsCB) self->settingsCB(self->devSettings);
  }, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t* hint = lv_label_create(settingsScreen);
  lv_label_set_text(hint, "swipe down to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  // Restart button (top right of the title row): opens the yes/no modal.
  {
    lv_obj_t* rbtn = lv_btn_create(settingsScreen);
    lv_obj_set_size(rbtn, 44, 36);
    lv_obj_align(rbtn, LV_ALIGN_TOP_MID, 128, 52);
    lv_obj_set_style_bg_color(rbtn, lv_color_hex(0x1A0A10), 0);
    lv_obj_set_style_radius(rbtn, 10, 0);
    lv_obj_set_style_border_width(rbtn, 2, 0);
    lv_obj_set_style_border_color(rbtn, lv_color_hex(0xE05060), 0);
    lv_obj_t* rlbl = lv_label_create(rbtn);
    lv_label_set_text(rlbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(rlbl, lv_color_hex(0xE05060), 0);
    lv_obj_center(rlbl);
    lv_obj_add_event_cb(rbtn, [](lv_event_t* e) {
      PetUI* self = (PetUI*)lv_event_get_user_data(e);
      if (lv_tick_get() - self->lastGestureMs < 600) return;
      lv_obj_clear_flag(self->setRestartOverlay, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, this);
  }

  // "Are you sure" modal: full-screen dim backdrop (swallows taps) + dialog.
  setRestartOverlay = lv_obj_create(settingsScreen);
  lv_obj_set_size(setRestartOverlay, 466, 466);
  lv_obj_set_pos(setRestartOverlay, 0, 0);
  lv_obj_set_style_bg_color(setRestartOverlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(setRestartOverlay, LV_OPA_80, 0);
  lv_obj_set_style_border_width(setRestartOverlay, 0, 0);
  lv_obj_set_style_radius(setRestartOverlay, 0, 0);
  lv_obj_clear_flag(setRestartOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(setRestartOverlay, LV_OBJ_FLAG_CLICKABLE);  // block the screen behind
  lv_obj_add_flag(setRestartOverlay, LV_OBJ_FLAG_HIDDEN);
  {
    lv_obj_t* q = lv_label_create(setRestartOverlay);
    lv_label_set_text(q, "restart the device?");
    lv_obj_set_style_text_color(q, lv_color_hex(0xE0E4F0), 0);
    lv_obj_align(q, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t* yes = lv_btn_create(setRestartOverlay);
    lv_obj_set_size(yes, 130, 52);
    lv_obj_align(yes, LV_ALIGN_CENTER, -75, 30);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x2A0A10), 0);
    lv_obj_set_style_radius(yes, 14, 0);
    lv_obj_set_style_border_width(yes, 2, 0);
    lv_obj_set_style_border_color(yes, lv_color_hex(0xE05060), 0);
    lv_obj_t* ylbl = lv_label_create(yes);
    lv_label_set_text(ylbl, LV_SYMBOL_OK "  yes");
    lv_obj_set_style_text_color(ylbl, lv_color_hex(0xE05060), 0);
    lv_obj_center(ylbl);
    lv_obj_add_event_cb(yes, [](lv_event_t* e) {
      PetUI* self = (PetUI*)lv_event_get_user_data(e);
      if (self->restartCB) self->restartCB();  // never returns (ESP.restart)
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* no = lv_btn_create(setRestartOverlay);
    lv_obj_set_size(no, 130, 52);
    lv_obj_align(no, LV_ALIGN_CENTER, 75, 30);
    lv_obj_set_style_bg_color(no, lv_color_hex(0x10101E), 0);
    lv_obj_set_style_radius(no, 14, 0);
    lv_obj_set_style_border_width(no, 2, 0);
    lv_obj_set_style_border_color(no, lv_color_hex(0x8A9AB8), 0);
    lv_obj_t* nlbl = lv_label_create(no);
    lv_label_set_text(nlbl, LV_SYMBOL_CLOSE "  no");
    lv_obj_set_style_text_color(nlbl, lv_color_hex(0x8A9AB8), 0);
    lv_obj_center(nlbl);
    lv_obj_add_event_cb(no, [](lv_event_t* e) {
      PetUI* self = (PetUI*)lv_event_get_user_data(e);
      lv_obj_add_flag(self->setRestartOverlay, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, this);
  }

  // WiFi + dashboard status (bottom): fed by the sketch via setWifiStatus.
  setWifiLabel = lv_label_create(settingsScreen);
  lv_obj_set_width(setWifiLabel, 320);
  lv_obj_set_style_text_align(setWifiLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(setWifiLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(setWifiLabel, lv_color_hex(0x5A6478), 0);
  lv_label_set_text(setWifiLabel, LV_SYMBOL_WIFI "  checking...");
  lv_obj_align(setWifiLabel, LV_ALIGN_TOP_MID, 0, 410);

  // The modal must dim everything, including widgets created after it.
  lv_obj_move_foreground(setRestartOverlay);

  lv_obj_add_event_cb(settingsScreen, settingsGestureCB, LV_EVENT_GESTURE, this);
}

// Highlight helper: selected option gets a colored border, others go dim.
static void settingsMarkSelected(lv_obj_t* const* btns, int n, int selected,
                                 uint32_t accent) {
  for (int i = 0; i < n; i++) {
    lv_obj_set_style_border_width(btns[i], 2, 0);
    lv_obj_set_style_border_color(btns[i],
        lv_color_hex(i == selected ? accent : 0x2A2A44), 0);
    if (lv_obj_get_child(btns[i], 0))
      lv_obj_set_style_text_color(lv_obj_get_child(btns[i], 0),
          lv_color_hex(i == selected ? accent : 0x6A6A88), 0);
    lv_obj_set_style_bg_color(btns[i], lv_color_hex(0x10101E), 0);
    lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
  }
}

void PetUI::applySettingsVisuals() {
  lv_slider_set_value(setVolSlider, devSettings.volumePct, LV_ANIM_OFF);
  lv_slider_set_value(setBriSlider, devSettings.brightness, LV_ANIM_OFF);
  settingsMarkSelected(setThemeBtns, 3, devSettings.soundTheme, 0x50A8E8);
  int sleepIdx = 1;
  for (int i = 0; i < 4; i++)
    if (SLEEP_CHOICES[i] == devSettings.sleepMin) sleepIdx = i;
  settingsMarkSelected(setSleepBtns, 4, sleepIdx, 0xA080FF);
  if (devSettings.stepsOn) lv_obj_add_state(setStepsSwitch, LV_STATE_CHECKED);
  else lv_obj_clear_state(setStepsSwitch, LV_STATE_CHECKED);
  if (devSettings.liftWake) lv_obj_add_state(setLiftSwitch, LV_STATE_CHECKED);
  else lv_obj_clear_state(setLiftSwitch, LV_STATE_CHECKED);
  // bg swatches keep their color fill; selection = border only
  for (int i = 0; i < 5; i++) {
    lv_obj_set_style_border_width(setBgBtns[i], 2, 0);
    lv_obj_set_style_border_color(setBgBtns[i],
        lv_color_hex(i == devSettings.petBg ? 0xFFD060 : 0x2A2A44), 0);
    lv_obj_set_style_bg_color(setBgBtns[i],
        lv_color_hex(i ? PET_BG_COLORS[i] : 0x14141E), 0);
  }
  lv_obj_set_style_bg_color(petScreen,
      lv_color_hex(PET_BG_COLORS[devSettings.petBg < 5 ? devSettings.petBg : 0]), 0);
}

void PetUI::setDeviceSettings(const DeviceSettings& s) {
  devSettings = s;
  if (devSettings.petBg >= 5) devSettings.petBg = 0;
  if (devSettings.soundTheme >= 3) devSettings.soundTheme = 0;
  if (devSettings.brightness < 20) devSettings.brightness = 20;
  if (devSettings.stepsOn > 1) devSettings.stepsOn = 1;
  if (devSettings.liftWake > 1) devSettings.liftWake = 1;
  applySettingsVisuals();
}

void PetUI::showSettingsScreen() {
  applySettingsVisuals();
  lv_obj_add_flag(setRestartOverlay, LV_OBJ_FLAG_HIDDEN);  // fresh open = no stale modal
  lv_scr_load_anim(settingsScreen, LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, false);
}

void PetUI::setVolSliderCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->devSettings.volumePct = (uint8_t)lv_slider_get_value(self->setVolSlider);
  if (self->settingsCB) self->settingsCB(self->devSettings);
  if (self->soundCB) self->soundCB(SOUND_HABIT_DONE);  // volume preview
}

void PetUI::setBriSliderCB(lv_event_t* e) {
  // VALUE_CHANGED (not RELEASED): brightness previews live while dragging.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->devSettings.brightness = (uint8_t)lv_slider_get_value(self->setBriSlider);
  if (self->settingsCB) self->settingsCB(self->devSettings);
}

void PetUI::setThemeBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  self->devSettings.soundTheme =
      (uint8_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  self->applySettingsVisuals();
  if (self->settingsCB) self->settingsCB(self->devSettings);
  if (self->soundCB) self->soundCB(SOUND_HABIT_DONE);  // theme preview
}

void PetUI::setBgBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  self->devSettings.petBg =
      (uint8_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  self->applySettingsVisuals();
  if (self->settingsCB) self->settingsCB(self->devSettings);
}

void PetUI::setSleepBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  self->devSettings.sleepMin = SLEEP_CHOICES[idx];
  self->applySettingsVisuals();
  if (self->settingsCB) self->settingsCB(self->devSettings);
}

void PetUI::setWifiStatus(bool connected, const char* text) {
  char buf[96];
  snprintf(buf, sizeof(buf), "%s  %s",
           connected ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING, text);
  // Change-guarded: the sketch pushes every few seconds; don't invalidate
  // the label (and redraw that strip) when nothing moved.
  if (strcmp(lv_label_get_text(setWifiLabel), buf) == 0) return;
  lv_label_set_text(setWifiLabel, buf);
  lv_obj_set_style_text_color(setWifiLabel,
    lv_color_hex(connected ? 0x3EE8A0 : 0xE05060), 0);
}

void PetUI::settingsGestureCB(lv_event_t* e) {
  // Opened with swipe up; swipe down closes.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  if (lv_indev_get_gesture_dir(lv_indev_get_act()) != LV_DIR_BOTTOM) return;
  self->showPetScreen();
}

/* ---- trophy screen ------------------------------------------------- */

void PetUI::buildTrophyScreen() {
  trophyScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(trophyScreen, lv_color_hex(0x000000), 0);

  lv_obj_t* title = lv_label_create(trophyScreen);
  lv_label_set_text(title, "Trophies");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFD060), 0);

  trophyList = lv_list_create(trophyScreen);
  lv_obj_set_size(trophyList, 350, 336);  // same max round-safe footprint as the habit list
  lv_obj_align(trophyList, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(trophyList, 30, 0);
  lv_obj_set_style_clip_corner(trophyList, true, 0);
  lv_obj_set_style_bg_color(trophyList, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(trophyList, lv_color_hex(0x3A2E08), 0);
  lv_obj_set_style_border_width(trophyList, 1, 0);
  lv_obj_set_style_pad_row(trophyList, 6, 0);
  lv_obj_set_style_pad_all(trophyList, 8, 0);
  lv_obj_set_scroll_dir(trophyList, LV_DIR_VER);  // keep horizontal swipes for the exit gesture

  lv_obj_t* hint = lv_label_create(trophyScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(trophyScreen, trophyGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::refreshTrophyScreen() {
  lv_obj_clean(trophyList);
  if (trophyCount == 0) {
    lv_obj_t* empty = lv_label_create(trophyList);
    lv_label_set_text(empty, "no trophies yet\n\nkeep at it!");
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x5A4A18), 0);
    lv_obj_center(empty);
    return;
  }
  for (int i = 0; i < trophyCount; i++) {
    lv_obj_t* btn = lv_list_add_btn(trophyList, LV_SYMBOL_OK, trophies[i].name);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x171204), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFD060), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);  // read-only: earned server-side
  }
}

void PetUI::showTrophyScreen() {
  refreshTrophyScreen();
  lv_scr_load_anim(trophyScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::setTrophies(const TrophyInfo* newTrophies, int count) {
  if (count < 0) count = 0;
  if (count > MAX_TROPHIES) count = MAX_TROPHIES;
  trophyCount = count;
  for (int i = 0; i < count; i++) trophies[i] = newTrophies[i];
  if (lv_scr_act() == trophyScreen) refreshTrophyScreen();
}

void PetUI::trophyGestureCB(lv_event_t* e) {
  // Same dismissal as the other menu apps: any swipe goes back to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->showPetScreen();
}

void PetUI::appsGestureCB(lv_event_t* e) {
  // Opened by a physical button, so there's no "opposite swipe" to mirror:
  // any swipe direction dismisses back to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->showPetScreen();
}

/* ---- sleep screen -------------------------------------------------- */

// Button styling: label text + accent color per quality, matching the
// workout difficulty palette (green / amber / red).
static const char*    SLEEP_LABELS[3] = { "good", "medium", "bad" };
static const uint32_t SLEEP_COLORS[3] = { 0x3EE8A0, 0xFFD060, 0xFF8080 };

void PetUI::buildSleepScreen() {
  sleepScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(sleepScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(sleepScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(sleepScreen);
  lv_label_set_text(title, "SLEEP");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(title, lv_color_hex(0x4A3A80), 0);

  lv_obj_t* prompt = lv_label_create(sleepScreen);
  lv_label_set_text(prompt, "how did you sleep?");
  lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_text_color(prompt, lv_color_hex(0xA080FF), 0);

  lv_obj_t* reward = lv_label_create(sleepScreen);
  lv_label_set_text(reward, "good: +12 mood +8 food   bad: -15 xp -10 mood");
  lv_obj_set_style_text_font(reward, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(reward, lv_color_hex(0x3A3060), 0);
  lv_obj_align(reward, LV_ALIGN_TOP_MID, 0, 354);

  const int btnH = 64, pitch = btnH + 16;
  for (int i = 0; i < 3; i++) {
    lv_obj_t* btn = lv_btn_create(sleepScreen);
    lv_obj_set_size(btn, 280, btnH);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, (i - 1) * pitch + 6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x10101E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(SLEEP_COLORS[i]), 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, SLEEP_LABELS[i]);
    lv_obj_set_style_text_color(lbl, lv_color_hex(SLEEP_COLORS[i]), 0);
    lv_obj_center(lbl);
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(btn, sleepBtnCB, LV_EVENT_CLICKED, this);
    sleepBtns[i] = btn;
  }

  sleepStatusLabel = lv_label_create(sleepScreen);
  lv_label_set_text(sleepStatusLabel, "");
  lv_obj_align(sleepStatusLabel, LV_ALIGN_BOTTOM_MID, 0, -76);
  lv_obj_set_style_text_color(sleepStatusLabel, lv_color_hex(0xA080FF), 0);

  lv_obj_t* hint = lv_label_create(sleepScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(sleepScreen, sleepGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::refreshSleepScreen() {
  for (int i = 0; i < 3; i++) {
    if (sleepLogged) {
      lv_obj_add_state(sleepBtns[i], LV_STATE_DISABLED);
      lv_obj_set_style_opa(sleepBtns[i], i == sleepQuality ? LV_OPA_COVER : LV_OPA_40, 0);
    } else {
      lv_obj_clear_state(sleepBtns[i], LV_STATE_DISABLED);
      lv_obj_set_style_opa(sleepBtns[i], LV_OPA_COVER, 0);
    }
  }
  if (sleepLogged) {
    lv_label_set_text_fmt(sleepStatusLabel, "logged today: %s", SLEEP_LABELS[sleepQuality]);
    lv_obj_set_style_text_color(sleepStatusLabel, lv_color_hex(SLEEP_COLORS[sleepQuality]), 0);
  } else {
    lv_label_set_text(sleepStatusLabel, "");
  }
}

void PetUI::showSleepScreen() {
  refreshSleepScreen();
  lv_scr_load_anim(sleepScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::setSleepLogged(bool logged, int quality) {
  sleepLogged  = logged;
  sleepQuality = (quality >= 0 && quality <= 2) ? quality : 0;
  if (lv_scr_act() == sleepScreen) refreshSleepScreen();
}

void PetUI::sleepBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  // Swipes emit a CLICKED on release; same guard as the habit list.
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  if (self->sleepLogged) return;
  int quality = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

  self->pet->logSleep(quality);
  self->sleepLogged  = true;
  self->sleepQuality = quality;
  self->refreshSleepScreen();
  self->refreshPetScreen();
  if (self->soundCB) self->soundCB(quality == 2 ? SOUND_HABIT_UNDONE : SOUND_HABIT_DONE);
  if (self->sleepCB) self->sleepCB(quality);

  // Brief pause so the "logged today" state registers, then back to the pet
  // so the mood/hunger change is visible immediately.
  lv_timer_t* t = lv_timer_create(sleepReturnTimerCB, 900, self);
  lv_timer_set_repeat_count(t, 1);
}

void PetUI::sleepReturnTimerCB(lv_timer_t* t) {
  PetUI* self = (PetUI*)t->user_data;
  if (lv_scr_act() == self->sleepScreen) self->showPetScreen();
}

void PetUI::sleepGestureCB(lv_event_t* e) {
  // Same dismissal as the apps menu: any swipe goes back to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->showPetScreen();
}

/* ---- walk screen -------------------------------------------------- */

void PetUI::buildWalkScreen() {
  walkScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(walkScreen, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(walkScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(walkScreen);
  lv_label_set_text(title, "WALK");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_set_style_text_color(title, lv_color_hex(0x204A68), 0);

  lv_obj_t* reward = lv_label_create(walkScreen);
  lv_label_set_text_fmt(reward, "%d steps: +%d xp", WALK_DAILY_GOAL, WALK_GOAL_XP);
  lv_obj_set_style_text_font(reward, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(reward, lv_color_hex(0x1E3A50), 0);
  lv_obj_align(reward, LV_ALIGN_TOP_MID, 0, 42);

  // Progress ring toward WALK_DAILY_GOAL, same construction as the focus ring.
  walkArc = lv_arc_create(walkScreen);
  lv_obj_set_size(walkArc, 320, 320);
  lv_obj_align(walkArc, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_rotation(walkArc, 270);       // visual start at 12 o'clock
  lv_arc_set_bg_angles(walkArc, 0, 359);   // full circle track (359° avoids wrap glitch)
  lv_arc_set_range(walkArc, 0, WALK_DAILY_GOAL);
  lv_arc_set_value(walkArc, 0);

  lv_obj_set_style_arc_color(walkArc, lv_color_hex(0x1C1C1C), LV_PART_MAIN);
  lv_obj_set_style_arc_width(walkArc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_color(walkArc, lv_color_hex(0x50A8E8), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(walkArc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_opa(walkArc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(walkArc, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(walkArc, LV_OBJ_FLAG_CLICKABLE);

  walkStepLabel = lv_label_create(walkScreen);
  lv_label_set_text(walkStepLabel, "0");
  lv_obj_set_style_text_font(walkStepLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(walkStepLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(walkStepLabel, LV_ALIGN_CENTER, 0, -34);

  walkCaptionLabel = lv_label_create(walkScreen);
  lv_label_set_text(walkCaptionLabel, "steps");
  lv_obj_set_style_text_color(walkCaptionLabel, lv_color_hex(0x2A4A68), 0);
  lv_obj_align(walkCaptionLabel, LV_ALIGN_CENTER, 0, 2);

  walkDistLabel = lv_label_create(walkScreen);
  lv_label_set_text(walkDistLabel, "0.00 km");
  lv_obj_set_style_text_color(walkDistLabel, lv_color_hex(0x50A8E8), 0);
  lv_obj_align(walkDistLabel, LV_ALIGN_CENTER, 0, 40);

  walkGoalLabel = lv_label_create(walkScreen);
  lv_label_set_text_fmt(walkGoalLabel, "goal %d", WALK_DAILY_GOAL);
  lv_obj_set_style_text_color(walkGoalLabel, lv_color_hex(0x2A4A68), 0);
  lv_obj_align(walkGoalLabel, LV_ALIGN_CENTER, 0, 74);

  // Pocket mode: blank the panel + ignore touch so the device can count
  // steps in a pocket without phantom taps; the BOOT button wakes it back
  // into this screen (board-wired via PocketModeCB).
  lv_obj_t* pocketBtn = lv_btn_create(walkScreen);
  lv_obj_set_size(pocketBtn, 210, 44);
  lv_obj_align(pocketBtn, LV_ALIGN_BOTTOM_MID, 0, -78);
  lv_obj_set_style_bg_color(pocketBtn, lv_color_hex(0x10101E), 0);
  lv_obj_set_style_bg_opa(pocketBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(pocketBtn, 14, 0);
  lv_obj_set_style_border_width(pocketBtn, 2, 0);
  lv_obj_set_style_border_color(pocketBtn, lv_color_hex(0x50A8E8), 0);
  lv_obj_t* pocketLbl = lv_label_create(pocketBtn);
  lv_label_set_text(pocketLbl, LV_SYMBOL_EYE_CLOSE "  pocket mode");
  lv_obj_set_style_text_color(pocketLbl, lv_color_hex(0x50A8E8), 0);
  lv_obj_set_style_text_font(pocketLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(pocketLbl);
  lv_obj_add_event_cb(pocketBtn, walkPocketBtnCB, LV_EVENT_CLICKED, this);

  lv_obj_t* hint = lv_label_create(walkScreen);
  lv_label_set_text(hint, "swipe to close");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);

  lv_obj_add_event_cb(walkScreen, walkGestureCB, LV_EVENT_GESTURE, this);
}

void PetUI::updateClock(int hour, int minute) {
  lv_label_set_text_fmt(clockLabel, "%02d:%02d", hour, minute);
}

void PetUI::walkPocketBtnCB(lv_event_t* e) {
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  // Swipes emit a CLICKED on release; same guard as the habit list.
  if (lv_tick_get() - self->lastGestureMs < 600) return;
  if (self->pocketCB) self->pocketCB();
}

void PetUI::refreshWalkScreen() {
  if (!walkSensorOk) {
    lv_label_set_text(walkStepLabel, "--");
    lv_label_set_text(walkCaptionLabel, "no motion sensor");
    lv_label_set_text(walkDistLabel, "");
    lv_arc_set_value(walkArc, 0);
    return;
  }
  lv_label_set_text_fmt(walkStepLabel, "%u", (unsigned)walkSteps);
  lv_label_set_text(walkCaptionLabel,
                    devSettings.stepsOn ? "steps" : "steps (counter off)");
  // steps × stride, shown in km with two decimals — integer math only.
  uint32_t meters = walkSteps * WALK_STRIDE_CM / 100;
  lv_label_set_text_fmt(walkDistLabel, "%u.%02u km",
                        (unsigned)(meters / 1000), (unsigned)(meters % 1000 / 10));
  uint32_t v = walkSteps > WALK_DAILY_GOAL ? WALK_DAILY_GOAL : walkSteps;
  lv_arc_set_value(walkArc, v);
  if (walkSteps >= WALK_DAILY_GOAL) {
    // Goal day: ring and count go green, goal line celebrates.
    lv_obj_set_style_arc_color(walkArc, lv_color_hex(0x3EE8A0), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(walkStepLabel, lv_color_hex(0x3EE8A0), 0);
    lv_label_set_text_fmt(walkGoalLabel, "goal reached!  +%d xp", WALK_GOAL_XP);
    lv_obj_set_style_text_color(walkGoalLabel, lv_color_hex(0x3EE8A0), 0);
  } else {
    lv_obj_set_style_arc_color(walkArc, lv_color_hex(0x50A8E8), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(walkStepLabel, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text_fmt(walkGoalLabel, "goal %d", WALK_DAILY_GOAL);
    lv_obj_set_style_text_color(walkGoalLabel, lv_color_hex(0x2A4A68), 0);
  }
}

void PetUI::showWalkScreen() {
  refreshWalkScreen();
  lv_scr_load_anim(walkScreen, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

void PetUI::setSteps(uint32_t stepsToday, bool sensorOk) {
  walkSteps    = stepsToday;
  walkSensorOk = sensorOk;
  if (lv_scr_act() == walkScreen) refreshWalkScreen();
}

void PetUI::walkGestureCB(lv_event_t* e) {
  // Same dismissal as the apps menu: any swipe goes back to the pet.
  PetUI* self = (PetUI*)lv_event_get_user_data(e);
  self->lastGestureMs = lv_tick_get();
  lv_indev_wait_release(lv_indev_get_act());
  self->showPetScreen();
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
  lv_label_set_text(pomModeLabel, "FOCUS  +25 xp/block");
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
  lv_label_set_text(exitHint, "swipe " LV_SYMBOL_UP " for Koko");
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
    lv_label_set_text(pomModeLabel, "FOCUS  +25 xp/block");
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
    lv_label_set_text(pomModeLabel, "FOCUS  +25 xp/block");
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
      if (self->focusDoneCB) self->focusDoneCB();

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
