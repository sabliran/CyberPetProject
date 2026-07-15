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

// Trophies are dashboard-owned and read-only on the device, same contract
// as quests/goals: the sync response's `trophies` array (earned names only,
// computed server-side from history) is copied here for display.
#define MAX_TROPHIES     44   // >= total defined server-side (40 as of July 2026)
#define TROPHY_NAME_LEN  40

// Back-workout app tuning: reps are counted by the board's IMU on wide
// swings (sketch calls addBackRep while isBackRunning); hitting the target
// feeds the blob and pays BACK_XP.
#define BACK_TARGET_REPS 10
#define BACK_XP          15

// Push-up app: reps are nose-taps on the screen (any tap while running);
// same reward shape as the back workout.
#define PUSH_TARGET_REPS 10
#define PUSH_XP          15
// 15+ reps in one session earns a second snack (extra feedWorkout meal).
#define PUSH_BONUS_REPS  15

// Pull-up app: reps are counted by the board's IMU with the device in a
// pocket (sketch calls addPullupRep while isPullupRunning). Lower target and
// higher XP than the other strength apps — pull-ups are the hard ones.
#define PULLUP_TARGET_REPS 5
#define PULLUP_XP          20

struct TrophyInfo {
  char name[TROPHY_NAME_LEN];
};

// Walk app tuning. Steps come from the board's pedometer (1.75B: QMI8658
// hardware step counter); the sketch feeds them in via setSteps(). The UI
// only displays — daily reset, persistence and the goal XP award live in the
// sketch so screenless/sensorless builds pay nothing.
#define WALK_DAILY_GOAL 6000
#define WALK_STRIDE_CM  70    // rough stride for the distance estimate
#define WALK_GOAL_XP    25    // one-time daily award when the goal is reached

// Sound events. The UI layer only *emits* these; the board sketch registers a
// player via setSoundCallback() (speaker/codec wiring is board-specific).
// Unset callback = silent, so the sim and speakerless boards need nothing.
enum PetSoundEvent {
  SOUND_HABIT_DONE = 0,
  SOUND_HABIT_UNDONE,
  SOUND_TROPHY,        // new trophy arrived in a sync — little fanfare
  SOUND_REP_BLIP,      // quiet blip per counted rep (pull-ups, push-ups)
  SOUND_MOVE_ALERT,    // insistent stand-up chime (sitting app; repeats until acked)
};
typedef void (*PetSoundCB)(int event);

// Sleep app: fired when the user rates last night (0 good, 1 medium, 2 bad).
// The UI applies the pet effects itself (Pet::logSleep); the board sketch
// uses this hook to persist the once-per-day gate + pet state to NVS.
// Unset = nothing persists (sim: gate resets every run, which is fine).
typedef void (*SleepLogCB)(int quality);

// Walk app pocket mode: fired by the on-screen "pocket mode" button. The
// board sketch owns what it means (1.75B: panel brightness to 0 + ignore
// touch until the BOOT button is pressed; step counting keeps running).
// Unset = the button does nothing (sim has no panel to blank).
typedef void (*PocketModeCB)(void);

// Settings-screen restart button: fired after the user confirms the yes/no
// dialog. The sketch saves state and calls ESP.restart().
typedef void (*RestartCB)(void);

// Sit app display power: the UI can't touch the panel (hardware-agnostic),
// so it asks the sketch. false = screen dark to save battery while the sit
// timer runs (BOOT turns it back on); true = wake it (the stand-up alert
// always fires this before chiming — a dark nag is no nag).
typedef void (*ScreenPowerCB)(bool on);

// Back-workout app: fired when a session ends with the target reached
// (after the pet reward is applied). The sketch bumps its lifetime session
// counter, persists it, and reports it in sync requests so the server can
// award back-workout trophies.
typedef void (*BackDoneCB)(void);

// Push-up app: same contract as BackDoneCB, for the push-up session counter.
typedef void (*PushDoneCB)(void);

// Pull-up app: same contract as BackDoneCB, for the pull-up session counter.
typedef void (*PullupDoneCB)(void);

// Focus app: fired when a 25-minute focus block completes (after XP award).
typedef void (*FocusDoneCB)(void);

// Device settings (swipe-up screen). Owned by the UI; the board sketch
// registers the callback to persist them and apply the hardware-facing ones
// (panel brightness, speaker volume, auto-sleep timeout). The UI applies the
// pet-screen background itself. Plain bytes: NVS-blobbed via memcmp guard.
struct DeviceSettings {
  uint8_t volumePct;   // 0-100 master volume 
  uint8_t soundTheme;  // 0 classic, 1 arcade, 2 soft
  uint8_t brightness;  // panel brightness 20-255
  uint8_t petBg;       // pet-screen background palette index (PET_BG_COLORS)
  uint8_t sleepMin;    // auto-sleep timeout in minutes (1/2/5/10)
};
typedef void (*SettingsChangedCB)(const DeviceSettings& s);

class PetUI {
public:
  // Call once after lvgl + display + touch are already initialized
  // (i.e. after the Waveshare example's own display/touch init code runs).
  void init(Pet* pet, HabitTracker* tracker);

  void refreshPetScreen();
  void refreshHabitScreen();
  void showPetScreen();
  void showHabitScreen();
  void startIdleWobble(); // called by roamArrivalCB (file-scope)

  void showPomodoroScreen();
  void pomodoroGuiltTrip();   // hook for IMU "picked up during focus" guilt-trip
  // True while a focus block is actively running — the sketch's IMU sampler
  // uses this to fire the guilt-trip when the device is picked up.
  bool isFocusRunning() const { return pomRunning && pomState == POM_FOCUS; }
  // Gold celebration pill; the sketch calls this when a sync brings a trophy
  // count higher than the last known one.
  void showTrophyPill();
  void sedentaryNudge();       // trigger sedentary state (also called by sim `s` key)
  void updateBattery(int pct, bool charging = false); // pct 0-100; -1 = unknown; call periodically from loop()
  void updateClock(int hour, int minute); // pet-screen clock; hidden until first call (sketch gates on clock validity)

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

  // Walk app: shows today's steps against WALK_DAILY_GOAL. setSteps() is
  // cheap when the walk screen isn't showing (stores + returns), so the
  // sketch can call it from every pedometer poll. sensorOk=false renders
  // the "no motion sensor" state instead of a count.
  void showWalkScreen();
  void setSteps(uint32_t stepsToday, bool sensorOk);
  void setPocketModeCallback(PocketModeCB cb) { pocketCB = cb; }
  void setScreenPowerCallback(ScreenPowerCB cb) { screenPowerCB = cb; }
  void setRestartCallback(RestartCB cb) { restartCB = cb; }

  // Back-workout app: rep counter driven by the board's IMU. The sketch's
  // swing detector calls addBackRep() while isBackRunning() — cheap no-op
  // checks otherwise. Sensorless builds just never count.
  void showBackScreen();
  void addBackRep();
  bool isBackRunning() const { return backRunning; }
  void setBackDoneCallback(BackDoneCB cb) { backDoneCB = cb; }

  // Pull-up app: same IMU-driven contract as the back workout — the sketch's
  // pull detector calls addPullupRep() while isPullupRunning().
  void showPullupScreen();
  void addPullupRep();
  bool isPullupRunning() const { return pullupRunning; }
  void setPullupDoneCallback(PullupDoneCB cb) { pullupDoneCB = cb; }

  // Clean-room app: 3-minute tidy sprint from the apps menu. Finishing the
  // full timer awards XP (no food — meals stay earned by exercise). The
  // sketch checks isCleanRunning() so auto-sleep can't fire mid-sprint while
  // the device lies untouched.
  void showCleanScreen();
  bool isCleanRunning() const { return cleanRunning; }

  // Sitting / move-reminder app: tell the device you're sitting and it nags
  // you to stand up at the chosen interval; the alert chime repeats every
  // minute until you tap that you moved, which re-arms the app (no auto-
  // restart — START begins the next round). The sketch inhibits auto-sleep
  // while isSitRunning() — a deep-sleeping device can't nag anyone.
  void showSitScreen();
  bool isSitRunning() const { return sitRunning; }

  // Settings screen (swipe up on the pet). setDeviceSettings restores the
  // NVS-loaded values at boot (also re-applies the pet background); the
  // callback fires on every user change.
  void showSettingsScreen();
  // WiFi/dashboard status line at the bottom of the settings screen. The UI
  // is hardware-agnostic, so the sketch pushes the state in (every few
  // seconds); green when the link is up, red otherwise. `text` may be two
  // lines: link status + dashboard reachability.
  void setWifiStatus(bool connected, const char* text);
  void setDeviceSettings(const DeviceSettings& s);
  const DeviceSettings& getDeviceSettings() const { return devSettings; }
  void setSettingsCallback(SettingsChangedCB cb) { settingsCB = cb; }

  // Push-up app: rep counting is pure touch (nose-taps), so the UI owns it
  // entirely; the sketch registers the session-done callback and drives the
  // session end from the physical BOOT button: while isPushRunning() a short
  // press calls finishPushSession() instead of opening the apps menu (no
  // on-screen DONE — mid-push-up it was a nose-tap mis-hit hazard).
  void showPushScreen();
  bool isPushRunning() const { return pushRunning; }
  void finishPushSession();
  void setPushDoneCallback(PushDoneCB cb) { pushDoneCB = cb; }
  void setFocusDoneCallback(FocusDoneCB cb) { focusDoneCB = cb; }

  // Trophy screen (apps menu). Same sync contract as quests/goals:
  // call setTrophies after every successful sync.
  void showTrophyScreen();
  void setTrophies(const TrophyInfo* trophies, int count);

  // Sleep app: "how did you sleep?" — three buttons, once per day.
  // setSleepLogged() is how the sketch restores/clears the daily gate
  // (boot restore from NVS, midnight re-arm); quality is only meaningful
  // when logged=true.
  void showSleepScreen();
  void setSleepLogged(bool logged, int quality);
  void setSleepCallback(SleepLogCB cb) { sleepCB = cb; }

  // Apps menu: a launcher listing the built-in apps (walk, back, sit, ...).
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
  lv_obj_t*    clockLabel;        // pet screen clock; empty until updateClock()
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
  lv_timer_t* walkAnimTimer;   // flips walk frames while gliding; null when idle
  bool        walkFrameB;
  int         depthZoom;       // resting image zoom (256 = 100%); roams pick a new depth

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

  // back-workout screen widgets + state
  lv_obj_t*  backScreen;
  lv_obj_t*  backRepLabel;
  lv_obj_t*  backRewardLabel;
  lv_obj_t*  backHintLabel;
  lv_obj_t*  backBtnLabel;
  int        backReps;
  bool       backRunning;
  BackDoneCB backDoneCB = nullptr;  // deliberately NOT reset in init()

  // settings screen widgets + state
  lv_obj_t*  settingsScreen;
  lv_obj_t*  setVolSlider;
  lv_obj_t*  setWifiLabel;
  lv_obj_t*  setRestartOverlay;  // "are you sure" modal, hidden by default
  lv_obj_t*  setBriSlider;
  lv_obj_t*  setThemeBtns[3];
  lv_obj_t*  setBgBtns[5];
  lv_obj_t*  setSleepBtns[4];
  DeviceSettings devSettings;          // defaults set in init()
  SettingsChangedCB settingsCB = nullptr;  // deliberately NOT reset in init()

  // pull-up screen widgets + state
  lv_obj_t*    pullupScreen;
  lv_obj_t*    pullupRepLabel;
  lv_obj_t*    pullupHintLabel;
  lv_obj_t*    pullupRewardLabel;
  lv_obj_t*    pullupBtnLabel;
  int          pullupReps;
  bool         pullupRunning;
  PullupDoneCB pullupDoneCB = nullptr;  // deliberately NOT reset in init()

  // sitting reminder screen widgets + state
  lv_obj_t*   sitScreen;
  lv_obj_t*   sitArc;
  lv_obj_t*   sitTimeLabel;
  lv_obj_t*   sitHintLabel;
  lv_obj_t*   sitBtnLabel;
  lv_obj_t*   sitChoiceBtns[4];
  lv_obj_t*   sitScreenOffBtn;   // visible only mid-session (not during alert)
  bool        sitRunning;
  bool        sitAlerting;
  uint8_t     sitIntervalMin;
  uint32_t    sitStartMs;
  uint32_t    sitLastChimeMs;
  lv_timer_t* sitClockTimer;
  void sitMarkChoice();
  void sitEnterAlert();
  void sitAckMoved();
  void sitStop();
  void sitUpdateAux();  // choice row vs screen-off button visibility
  // "gives: ..." blurb under the workout titles; snack numbers follow the
  // dashboard difficulty so they're refreshed on every screen open.
  void setWorkoutReward(lv_obj_t* lbl, int xp, bool pushBonus);

  // clean-room screen widgets + state
  lv_obj_t*   cleanScreen;
  lv_obj_t*   cleanArc;
  lv_obj_t*   cleanTimeLabel;
  lv_obj_t*   cleanHintLabel;
  lv_obj_t*   cleanBtnLabel;
  bool        cleanRunning;
  uint32_t    cleanStartMs;
  lv_timer_t* cleanClockTimer;

  // push-up screen widgets + state
  lv_obj_t*  pushScreen;
  lv_obj_t*  pushRepLabel;
  lv_obj_t*  pushHintLabel;
  lv_obj_t*  pushRewardLabel;
  lv_obj_t*  pushStartBtn;          // hidden while a session runs (BOOT ends it)
  lv_obj_t*  pushBtnLabel;
  int        pushReps;
  bool       pushRunning;
  uint32_t   lastPushTapMs;         // debounce for nose taps
  PushDoneCB pushDoneCB = nullptr;  // deliberately NOT reset in init()
  FocusDoneCB focusDoneCB = nullptr;  // deliberately NOT reset in init()

  // trophy screen widgets + data
  lv_obj_t*   trophyScreen;
  lv_obj_t*   trophyList;
  TrophyInfo  trophies[MAX_TROPHIES];
  int         trophyCount;

  // sleep screen widgets + state
  lv_obj_t*  sleepScreen;
  lv_obj_t*  sleepBtns[3];
  lv_obj_t*  sleepStatusLabel;
  bool       sleepLogged;
  int        sleepQuality;
  SleepLogCB sleepCB = nullptr;  // deliberately NOT reset in init()

  // walk screen widgets + state
  lv_obj_t*  walkScreen;
  lv_obj_t*  walkArc;
  lv_obj_t*  walkStepLabel;
  lv_obj_t*  walkCaptionLabel;
  lv_obj_t*  walkDistLabel;
  lv_obj_t*  walkGoalLabel;
  uint32_t   walkSteps;
  bool       walkSensorOk;
  PocketModeCB pocketCB = nullptr;  // deliberately NOT reset in init()
  ScreenPowerCB screenPowerCB = nullptr;  // deliberately NOT reset in init()
  RestartCB restartCB = nullptr;          // deliberately NOT reset in init()

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
  lv_obj_t*  healthLabel;
  lv_obj_t*  healthBar;
  void positionHealthBar();                     // pin the HP bar above the sprite
  static void healthFollowCB(lv_timer_t* t);    // ~12 Hz follower while pet screen shown

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
  void buildBackScreen();
  void buildPullupScreen();
  void buildCleanScreen();
  void buildSitScreen();
  void buildSettingsScreen();
  void applySettingsVisuals();  // sync widgets + pet bg to devSettings
  void buildPushScreen();
  void buildTrophyScreen();
  void refreshTrophyScreen();
  void buildSleepScreen();
  void refreshSleepScreen();
  void buildWalkScreen();
  void refreshWalkScreen();
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
  static void cleanBtnCB(lv_event_t* e);
  static void cleanGestureCB(lv_event_t* e);
  static void cleanClockCB(lv_timer_t* t);
  static void sitBtnCB(lv_event_t* e);
  static void sitChoiceCB(lv_event_t* e);
  static void sitTapCB(lv_event_t* e);
  static void sitGestureCB(lv_event_t* e);
  static void sitClockCB(lv_timer_t* t);
  static void pomGuiltRevertCB(lv_timer_t* t);
  static void sedentaryCheckCB(lv_timer_t* t);

  static void exprTimerCB(lv_timer_t* t);
  static void revertExprCB(lv_timer_t* t);
  static void roamTimerCB(lv_timer_t* t);
  static void blinkTimerCB(lv_timer_t* t);
  static void walkFrameCB(lv_timer_t* t);
  static void animSetDepth(void* petui, int32_t v);  // zoom + eye rescale per frame
  static void habitButtonEventCB(lv_event_t* e);
  // Gesture navigation: pet screen is the hub. Swipe left = habits,
  // down = focus; the opposite swipe exits back to the pet.
  static void petGestureCB(lv_event_t* e);
  static void habitGestureCB(lv_event_t* e);
  static void questGestureCB(lv_event_t* e);
  static void goalGestureCB(lv_event_t* e);
  static void pomGestureCB(lv_event_t* e);
  static void appsGestureCB(lv_event_t* e);
  static void appsBtnCB(lv_event_t* e);
  static void walkGestureCB(lv_event_t* e);
  static void walkPocketBtnCB(lv_event_t* e);
  static void trophyGestureCB(lv_event_t* e);
  static void backBtnCB(lv_event_t* e);
  static void backGestureCB(lv_event_t* e);
  static void backDoneTimerCB(lv_timer_t* t);
  static void pullupBtnCB(lv_event_t* e);
  static void pullupGestureCB(lv_event_t* e);
  static void pullupDoneTimerCB(lv_timer_t* t);
  static void settingsGestureCB(lv_event_t* e);
  static void setVolSliderCB(lv_event_t* e);
  static void setBriSliderCB(lv_event_t* e);
  static void setThemeBtnCB(lv_event_t* e);
  static void setBgBtnCB(lv_event_t* e);
  static void setSleepBtnCB(lv_event_t* e);
  static void pushBtnCB(lv_event_t* e);
  static void pushTapCB(lv_event_t* e);
  static void pushGestureCB(lv_event_t* e);
  static void pushDoneTimerCB(lv_timer_t* t);
  static void sleepBtnCB(lv_event_t* e);
  static void sleepGestureCB(lv_event_t* e);
  static void sleepReturnTimerCB(lv_timer_t* t);
  static void petLongPressCB(lv_event_t* e);    // press & hold = manual sync
  static void blobPatCB(lv_event_t* e);         // quick tap on the blob = love reaction
  static void pomTapCB(lv_event_t* e);          // double-tap = play/pause on focus screen
  static void syncTimeoutCB(lv_timer_t* t);
  static void syncOverlayDeleteCB(lv_anim_t* a);
};
