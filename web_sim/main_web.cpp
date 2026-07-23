/*
  CyberPet web simulator entry point.

  Keyboard shortcuts:
    d     - advance one day  (dailyTick + habit reset)
    x     - +25 XP
    h     - advance one hour of hunger decay
    m     - toggle dev menu overlay
    space - count a rep (when workout screen is running)
    p     - simulate IMU pickup during Pomodoro focus (guilt-trip flash)
    g     - open/close the dictionary app (wheel -> list -> definition;
            needs tools/dict_out generated - see tools/make_dict.py)
*/

#include <SDL2/SDL.h>
#include "lvgl.h"
#include "pet.h"
#include "habits.h"
#include "ui.h"
#include "dict_ui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static const int HOR_RES = 466;
static const int VER_RES = 466;

static SDL_Window*   window   = nullptr;
static SDL_Renderer* renderer = nullptr;
static SDL_Texture*  texture  = nullptr;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;

static bool mouse_pressed = false;
static int  mouse_x = 0, mouse_y = 0;
static bool quit_requested = false;

Pet pet;
HabitTracker habits;
PetUI ui;

/* ================================================================
   DEV MENU — simulator only, toggled with 'm'
   ================================================================ */

static lv_obj_t* devPanel    = nullptr;
static lv_obj_t* devInfoLbl  = nullptr;
static bool      devOpen     = false;

static void refreshDevMenu() {
  if (!devInfoLbl) return;
  int level  = pet.getLevel();
  int xpIn   = pet.xpIntoCurrentLevel();
  int xpNeed = pet.xpToNextLevel();
  const char* aliveStr = pet.isAlive() ? "" : "  [DEAD]";
  lv_label_set_text_fmt(devInfoLbl,
    "Stage: %s   LV.%d%s\n"
    "XP: %d/%d to next\n"
    "Mood: %d / 100\n"
    "Hunger: %d / 100",
    pet.getStageName(), level, aliveStr,
    xpIn, xpIn + xpNeed,
    pet.getMood(),
    pet.getHunger());
}

// Action IDs packed into button user_data
enum DevAction {
  DA_XP_100, DA_XP_500, DA_XP_1000, DA_LEVEL_UP,
  DA_TO_BLOB, DA_TO_CREATURE, DA_TO_EVOLVED,
  DA_MOOD_0, DA_MOOD_50, DA_MOOD_100,
  DA_FEED, DA_HUNGER_50, DA_STARVE,
  DA_NEXT_DAY, DA_RESET, DA_CLOSE
};

static void devBtnCB(lv_event_t* e) {
  DevAction act = (DevAction)(intptr_t)lv_event_get_user_data(e);
  switch (act) {
    case DA_XP_100:      pet.addXP(100);  break;
    case DA_XP_500:      pet.addXP(500);  break;
    case DA_XP_1000:     pet.addXP(1000); break;
    case DA_LEVEL_UP:    pet.addXP(pet.xpToNextLevel()); break;
    case DA_TO_BLOB:     pet.setXP(Pet::xpForLevel(STAGE_LEVEL_THRESHOLDS[STAGE_BLOB]));     break;
    case DA_TO_CREATURE: pet.setXP(Pet::xpForLevel(STAGE_LEVEL_THRESHOLDS[STAGE_CREATURE])); break;
    case DA_TO_EVOLVED:  pet.setXP(Pet::xpForLevel(STAGE_LEVEL_THRESHOLDS[STAGE_EVOLVED]));  break;
    case DA_MOOD_0:      pet.setMood(0);   break;
    case DA_MOOD_50:     pet.setMood(50);  break;
    case DA_MOOD_100:    pet.setMood(100); break;
    case DA_FEED:        pet.feed();       break;
    case DA_HUNGER_50:   pet.setHunger(50); break;
    case DA_STARVE:      pet.setHunger(0);  break;
    case DA_NEXT_DAY:
      pet.dailyTick(habits.anyDoneToday());
      habits.resetDaily();
      ui.refreshHabitScreen();
      break;
    case DA_RESET:
      pet.setXP(0);
      pet.setMood(80);
      break;
    case DA_CLOSE:
      devOpen = false;
      lv_obj_add_flag(devPanel, LV_OBJ_FLAG_HIDDEN);
      return;
  }
  ui.refreshPetScreen();
  refreshDevMenu();
}

// Helper: create a styled dev button
static lv_obj_t* devBtn(lv_obj_t* parent, const char* label,
                         DevAction action, lv_color_t bg, lv_color_t fg) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_style_bg_color(btn, bg, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_pad_all(btn, 6, 0);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, fg, 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, devBtnCB, LV_EVENT_CLICKED, (void*)(intptr_t)action);
  return btn;
}

static void createDevMenu() {
  // Panel floats above all screens via the top layer
  devPanel = lv_obj_create(lv_layer_top());
  lv_obj_set_size(devPanel, 400, 468);
  lv_obj_align(devPanel, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(devPanel, lv_color_hex(0x06060F), 0);
  lv_obj_set_style_bg_opa(devPanel, LV_OPA_90, 0);
  lv_obj_set_style_border_color(devPanel, lv_color_hex(0x3030AA), 0);
  lv_obj_set_style_border_width(devPanel, 1, 0);
  lv_obj_set_style_radius(devPanel, 12, 0);
  lv_obj_set_style_pad_all(devPanel, 10, 0);
  lv_obj_clear_flag(devPanel, LV_OBJ_FLAG_SCROLLABLE);

  // Title
  lv_obj_t* title = lv_label_create(devPanel);
  lv_label_set_text(title, "DEV MENU");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x8888FF), 0);

  // Live info block
  devInfoLbl = lv_label_create(devPanel);
  lv_obj_align(devInfoLbl, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_set_style_text_color(devInfoLbl, lv_color_hex(0x9090AA), 0);
  lv_label_set_text(devInfoLbl, "");

  // Color palette for button groups
  lv_color_t bgXP   = lv_color_hex(0x07180F); lv_color_t fgXP   = lv_color_hex(0x3EE8A0);
  lv_color_t bgStg  = lv_color_hex(0x100820); lv_color_t fgStg  = lv_color_hex(0xAA88FF);
  lv_color_t bgMood = lv_color_hex(0x081020); lv_color_t fgMood = lv_color_hex(0x5599FF);
  lv_color_t bgUtil = lv_color_hex(0x141414); lv_color_t fgUtil = lv_color_hex(0x888888);
  lv_color_t bgRed  = lv_color_hex(0x1A0808); lv_color_t fgRed  = lv_color_hex(0xFF6666);

  // Row 1: XP buttons
  lv_obj_t* r1[] = {
    devBtn(devPanel, "+100xp",   DA_XP_100,  bgXP, fgXP),
    devBtn(devPanel, "+500xp",   DA_XP_500,  bgXP, fgXP),
    devBtn(devPanel, "+1000xp",  DA_XP_1000, bgXP, fgXP),
  };
  for (int i = 0; i < 3; i++) {
    lv_obj_set_size(r1[i], 110, 38);
    lv_obj_align(r1[i], LV_ALIGN_TOP_LEFT, i * 118, 92);
  }

  // Row 2: Level up + stage jump
  lv_obj_t* lvUp = devBtn(devPanel, "Level Up",    DA_LEVEL_UP,    bgXP,  fgXP);
  lv_obj_t* toB  = devBtn(devPanel, LV_SYMBOL_RIGHT " Blob",     DA_TO_BLOB,     bgStg, fgStg);
  lv_obj_t* toC  = devBtn(devPanel, LV_SYMBOL_RIGHT " Creature", DA_TO_CREATURE, bgStg, fgStg);
  lv_obj_t* toE  = devBtn(devPanel, LV_SYMBOL_RIGHT " Evolved",  DA_TO_EVOLVED,  bgStg, fgStg);
  lv_obj_set_size(lvUp, 110, 38); lv_obj_align(lvUp, LV_ALIGN_TOP_LEFT,   0, 140);
  lv_obj_set_size(toB,   80, 38); lv_obj_align(toB,  LV_ALIGN_TOP_LEFT, 118, 140);
  lv_obj_set_size(toC,   90, 38); lv_obj_align(toC,  LV_ALIGN_TOP_LEFT, 206, 140);
  lv_obj_set_size(toE,   90, 38); lv_obj_align(toE,  LV_ALIGN_TOP_LEFT, 304, 140);

  // Row 3: Mood
  lv_color_t bgFood = lv_color_hex(0x0F2010); lv_color_t fgFood = lv_color_hex(0x3EE8A0);
  lv_obj_t* m0  = devBtn(devPanel, "Mood 0",  DA_MOOD_0,   bgMood, fgMood);
  lv_obj_t* m50 = devBtn(devPanel, "Mood 50", DA_MOOD_50,  bgMood, fgMood);
  lv_obj_t* m100= devBtn(devPanel, "Mood 100",DA_MOOD_100, bgMood, fgMood);
  lv_obj_set_size(m0,   110, 38); lv_obj_align(m0,  LV_ALIGN_TOP_LEFT,   0, 188);
  lv_obj_set_size(m50,  110, 38); lv_obj_align(m50, LV_ALIGN_TOP_LEFT, 118, 188);
  lv_obj_set_size(m100, 110, 38); lv_obj_align(m100,LV_ALIGN_TOP_LEFT, 236, 188);

  // Row 4: Hunger
  lv_obj_t* hFeed   = devBtn(devPanel, LV_SYMBOL_OK " Feed",    DA_FEED,      bgFood,              fgFood);
  lv_obj_t* hHalf   = devBtn(devPanel, "Hunger 50",             DA_HUNGER_50, lv_color_hex(0x2A1800), lv_color_hex(0xD4A030));
  lv_obj_t* hStarve = devBtn(devPanel, LV_SYMBOL_WARNING " Starve", DA_STARVE, lv_color_hex(0x220000), lv_color_hex(0xFF5050));
  lv_obj_set_size(hFeed,   110, 38); lv_obj_align(hFeed,   LV_ALIGN_TOP_LEFT,   0, 236);
  lv_obj_set_size(hHalf,   110, 38); lv_obj_align(hHalf,   LV_ALIGN_TOP_LEFT, 118, 236);
  lv_obj_set_size(hStarve, 110, 38); lv_obj_align(hStarve, LV_ALIGN_TOP_LEFT, 236, 236);

  // Row 5: Day advance + reset
  lv_obj_t* day = devBtn(devPanel, LV_SYMBOL_REFRESH " Next Day", DA_NEXT_DAY, bgUtil, fgUtil);
  lv_obj_t* rst = devBtn(devPanel, LV_SYMBOL_TRASH " Reset Pet",  DA_RESET,    bgRed,  fgRed);
  lv_obj_set_size(day, 170, 38); lv_obj_align(day, LV_ALIGN_TOP_LEFT,   0, 284);
  lv_obj_set_size(rst, 170, 38); lv_obj_align(rst, LV_ALIGN_TOP_LEFT, 185, 284);

  // Close
  lv_obj_t* close = devBtn(devPanel, "Close  [M]", DA_CLOSE, lv_color_hex(0x0D0D22), lv_color_hex(0x6868AA));
  lv_obj_set_size(close, 360, 42);
  lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -4);

  lv_obj_add_flag(devPanel, LV_OBJ_FLAG_HIDDEN);
}

static void toggleDevMenu() {
  devOpen = !devOpen;
  if (devOpen) {
    refreshDevMenu();
    lv_obj_clear_flag(devPanel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(devPanel, LV_OBJ_FLAG_HIDDEN);
  }
}

/* ================================================================
   SDL display & input
   ================================================================ */

static void sdl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  SDL_Rect r;
  r.x = area->x1; r.y = area->y1;
  r.w = area->x2 - area->x1 + 1;
  r.h = area->y2 - area->y1 + 1;
  SDL_UpdateTexture(texture, &r, color_p, r.w * sizeof(lv_color_t));
  lv_disp_flush_ready(drv);
}

static void render_present() {
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, nullptr, nullptr);
  SDL_RenderPresent(renderer);
}

static void sdl_mouse_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;
  data->point.x = mouse_x;
  data->point.y = mouse_y;
  data->state = mouse_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void handle_sdl_events() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        quit_requested = true;
        break;
      case SDL_MOUSEMOTION:
        mouse_x = e.motion.x; mouse_y = e.motion.y;
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT) mouse_pressed = true;
        break;
      case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT) mouse_pressed = false;
        break;
      case SDL_KEYDOWN:
        switch (e.key.keysym.sym) {
          case SDLK_d:
            pet.dailyTick(habits.anyDoneToday());
            habits.resetDaily();
            ui.refreshPetScreen();
            ui.refreshHabitScreen();
            break;
          case SDLK_x:
            pet.addXP(25);
            ui.refreshPetScreen();
            break;
          case SDLK_h:
            pet.hungerHourlyTick();  // simulate one hour of hunger decay
            ui.refreshPetScreen();
            break;
          case SDLK_m:
            toggleDevMenu();
            break;
          case SDLK_s:
            ui.sedentaryNudge();
            break;
          case SDLK_b: {
            static int simBattState = 0;  // 0=full, 1=low, 2=charging
            simBattState = (simBattState + 1) % 3;
            if      (simBattState == 1) ui.updateBattery(10);
            else if (simBattState == 2) ui.updateBattery(60, true);
            else                        ui.updateBattery(100);
            break;
          }
          case SDLK_p:
            ui.pomodoroGuiltTrip();
            break;
          case SDLK_a:
            // Stands in for the board's BOOT button: exits the dictionary
            // when it's open (its only exit), apps menu otherwise.
            if (dictScreenActive()) hideDictScreen();
            else                    ui.showAppsMenu();
            break;
          case SDLK_g:
            // Dictionary toggle. The real navigation is the pet screen's
            // swipe-down (mouse-drag down); this shortcut stays as a way to
            // open it from any screen without swiping.
            if (dictScreenActive()) hideDictScreen();
            else                    showDictScreen();
            break;
        }
        break;
    }
  }
}

static void main_loop_iter() {
  static uint32_t last_tick = SDL_GetTicks();
  uint32_t now = SDL_GetTicks();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  handle_sdl_events();

  // Press-and-hold manual sync: no dashboard in the sim, so fake a successful sync
  // after a short spinner delay to exercise the overlay animation.
  if (ui.consumeSyncRequest()) {
    lv_timer_t* t = lv_timer_create(
        [](lv_timer_t* tt) { ui.syncFinished(true); }, 1200, nullptr);
    lv_timer_set_repeat_count(t, 1);  // one-shot; auto-deletes after firing
  }

  lv_timer_handler();
  render_present();
}

int main() {
  SDL_Init(SDL_INIT_VIDEO);
  window = SDL_CreateWindow("CyberPet Simulator",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            HOR_RES, VER_RES, 0);
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  texture  = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STREAMING, HOR_RES, VER_RES);
  lv_init();

  buf1 = (lv_color_t*)malloc(HOR_RES * VER_RES * sizeof(lv_color_t));
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, HOR_RES * VER_RES);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = HOR_RES;
  disp_drv.ver_res = VER_RES;
  disp_drv.flush_cb = sdl_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = sdl_mouse_read_cb;
  // Match the device's tap-vs-scroll threshold (see CyberPet_1_75B.ino:
  // finger taps wobble past the 10 px default and ate roller letter taps).
  indev_drv.scroll_limit = 25;
  lv_indev_drv_register(&indev_drv);

  habits.init();
  PetState fresh = pet.getState();
  pet.init(fresh);
  ui.init(&pet, &habits);

  // Demo quests (on-device these come from the dashboard sync response).
  static const QuestInfo demoQuests[] = {
    { "Read 10 pages",     30 },
    { "Inbox zero",        25 },
    { "Call grandma",      40 },
  };
  ui.setQuests(demoQuests, 3);

  // Demo goals (on-device these come from the dashboard sync response).
  static const GoalInfo demoGoals[] = {
    { "Run 20km",         50, "weekly" },
    { "No sugar",         15, "daily"  },
  };
  ui.setGoals(demoGoals, 2);

  createDevMenu();  // must be after lv_init and display registration

  // Round-glass mask: the real device is a circular AMOLED, so anything in
  // the square corners is physically invisible. Emulate that with a thick
  // circular border ring on the sys layer (always on top, never clickable).
  {
    lv_obj_t* mask = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(mask, HOR_RES + 400, VER_RES + 400);
    lv_obj_set_pos(mask, -200, -200);
    lv_obj_set_style_radius(mask, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(mask, 200, 0);
    lv_obj_set_style_border_opa(mask, LV_OPA_COVER, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(main_loop_iter, 0, 1);
#else
  while (!quit_requested) {
    main_loop_iter();
    SDL_Delay(5);
  }
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
#endif
  return 0;
}
