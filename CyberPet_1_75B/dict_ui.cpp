#include "dict_ui.h"
#include "dict.h"
#include <stdio.h>
#include <string.h>

// Dictionary UI (see dict_ui.h). Palette + layout idiom mirror ui.cpp: black
// screens, 0x3EE8A0 accent, 0x0D0D1F/0x0D0D22 panels, 0x1A1A2E/0x2A2A66
// borders, 0x8A9ACC/0x6868AA text, round-bezel-safe footprints, FADE_ON 150.

static const int      SEARCH_CAP  = 200;  // prefix count above this blocks SEARCH
static const int      LIST_ROWS   = 50;   // word-list rows incl. pinned exact
static const int      MIN_LETTERS = 2;    // shortest searchable word (generator drops 1-letter keys)
static const uint32_t COL_ACCENT  = 0x3EE8A0;
static const uint32_t COL_PANEL   = 0x0D0D1F;
static const uint32_t COL_PANEL2  = 0x0D0D22;
static const uint32_t COL_BORDER  = 0x1A1A2E;
static const uint32_t COL_BORDER2 = 0x2A2A66;
static const uint32_t COL_TEXT    = 0x8A9ACC;
static const uint32_t COL_DIM     = 0x6868AA;

static lv_obj_t* wheelScr   = NULL;
static lv_obj_t* listScr    = NULL;
static lv_obj_t* defScr     = NULL;
static lv_obj_t* failScr    = NULL;
static lv_obj_t* prevScreen = NULL;  // where back/hide returns to
static lv_obj_t* wordLabel  = NULL;
static lv_obj_t* matchLabel = NULL;
static lv_obj_t* searchBtn  = NULL;
static lv_obj_t* rollerL    = NULL;  // twin A-Z rollers: park each in a
static lv_obj_t* rollerR    = NULL;  // different alphabet region, tap appends

static char     word[DICT_KEY_LEN + 1];
static int      wordLen  = 0;
static uint16_t pressSel = 0;  // roller selection at press time ("moved" guard)

/* ---- shared widgets ---------------------------------------------------- */

static lv_obj_t* makeScreen() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

static lv_obj_t* makeBtn(lv_obj_t* parent, const char* text, uint32_t color,
                         lv_event_cb_t cb) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x10101E), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 16, 0);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(color), 0);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  return btn;
}

// Bottom-mid back button, round-bezel safe (same slot as ui.cpp's hints).
static void makeBackBtn(lv_obj_t* parent, lv_event_cb_t cb) {
  lv_obj_t* btn = makeBtn(parent, LV_SYMBOL_LEFT " back", COL_BORDER2, cb);
  lv_obj_set_size(btn, 120, 44);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_t* lbl = lv_obj_get_child(btn, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
}

/* ---- screen 1: letter wheel -------------------------------------------- */

static void updateWheel() {
  // Word-so-far with a trailing cursor so an empty word still reads as an
  // input line.
  char buf[DICT_KEY_LEN + 2];
  snprintf(buf, sizeof(buf), "%s_", word);
  lv_label_set_text(wordLabel, buf);

  // Match line + SEARCH enable, exactly the spec's five states.
  char line[96];
  bool enable = false;
  if (wordLen == 0) {
    snprintf(line, sizeof(line), "%d words", (int)DICT_WORD_COUNT);
  } else {
    uint32_t first = 0, count = 0;
    dictPrefixRange(word, &first, &count);
    if (wordLen < MIN_LETTERS) {
      snprintf(line, sizeof(line), "%u matches - add 1 more letter",
               (unsigned)count);
    } else {
      int exact = dictExact(word);
      enable = (count <= (uint32_t)SEARCH_CAP) || exact >= 0;
      if (count > (uint32_t)SEARCH_CAP && exact >= 0)
        snprintf(line, sizeof(line), "%u matches - '%s' is a word",
                 (unsigned)count, word);
      else if (count > (uint32_t)SEARCH_CAP)
        snprintf(line, sizeof(line), "%u matches - keep spelling",
                 (unsigned)count);
      else
        snprintf(line, sizeof(line), "%u matches", (unsigned)count);
    }
  }
  lv_label_set_text(matchLabel, line);

  if (enable) lv_obj_clear_state(searchBtn, LV_STATE_DISABLED);
  else        lv_obj_add_state(searchBtn, LV_STATE_DISABLED);
}

// Shared by both rollers; a single pressSel is safe because press/click
// always arrive as a pair from the same (single-touch) interaction.
static void rollerEventCB(lv_event_t* e) {
  lv_obj_t* r = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    pressSel = lv_roller_get_selected(r);
  } else if (code == LV_EVENT_CLICKED) {
    // The "moved" guard: a click that changed the roller value (tapping a
    // neighboring letter scrolls to it) only selects — a second click on
    // the now-centered letter appends it.
    if (lv_roller_get_selected(r) != pressSel) return;
    if (wordLen >= DICT_KEY_LEN) return;
    word[wordLen++] = 'a' + (char)lv_roller_get_selected(r);
    word[wordLen]   = '\0';
    updateWheel();
  }
}

static void deleteBtnCB(lv_event_t* e) {
  (void)e;
  if (wordLen > 0) word[--wordLen] = '\0';
  updateWheel();
}

static void clearBtnCB(lv_event_t* e) {
  (void)e;
  wordLen = 0;
  word[0] = '\0';
  updateWheel();
}

static void buildListScreen();  // fwd (SEARCH -> screen 2)

static void searchBtnCB(lv_event_t* e) {
  (void)e;
  buildListScreen();
  lv_scr_load_anim(listScr, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

// NO gesture handler on ANY dictionary screen (July 2026, user request):
// swipes here are roller flicks and list scrolls, and an accidental gesture
// must never quit mid-search — same trap the push-up/squat screens fixed.
// The physical BOOT button is the only exit (board sketch checks
// dictScreenActive() before its apps-menu fallback; the sim's 'a' key
// mirrors it). Internal back buttons only walk the dict's own screens.
static void buildWheelScreen() {
  wheelScr = makeScreen();

  // Same dim hint slot as other screens (bottom circle is ~148 px wide at
  // this y; 14 pt fits), same wording as the push/squat BOOT contract.
  lv_obj_t* exitHint = lv_label_create(wheelScr);
  lv_label_set_text(exitHint, "BOOT button = exit");
  lv_obj_align(exitHint, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_style_text_color(exitHint, lv_color_hex(0x2A2A44), 0);
  lv_obj_set_style_text_font(exitHint, &lv_font_montserrat_14, 0);

  wordLabel = lv_label_create(wheelScr);
  lv_obj_set_style_text_font(wordLabel, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(wordLabel, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_letter_space(wordLabel, 6, 0);
  lv_obj_set_width(wordLabel, 380);
  lv_label_set_long_mode(wordLabel, LV_LABEL_LONG_DOT);  // very long words
  lv_obj_set_style_text_align(wordLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(wordLabel, LV_ALIGN_TOP_MID, 0, 50);

  matchLabel = lv_label_create(wheelScr);
  lv_obj_set_style_text_font(matchLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(matchLabel, lv_color_hex(COL_TEXT), 0);
  lv_obj_align(matchLabel, LV_ALIGN_TOP_MID, 0, 96);

  // Twin rollers, montserrat_32 letters (user request: bigger + a second
  // wheel). Park each in a different alphabet region to halve scroll
  // travel; a tap on either appends its centered letter. 110 px wide each
  // at center ±63: outer edges ±118 from center, well inside the glass.
  static char opts[26 * 2];  // "A\nB\n...\nZ"
  char* p = opts;
  for (char c = 'A'; c <= 'Z'; c++) {
    *p++ = c;
    *p++ = (c == 'Z') ? '\0' : '\n';
  }
  // Fill the whole band between the match line (~y 118) and the buttons
  // (~y 360): 5 visible rows of montserrat_32 with extra line space ≈ 230
  // px tall. INFINITE mode wraps Z->A so no scroll-back across the
  // alphabet, and the taller rows are easier to land on.
  lv_obj_t** rollers[2] = { &rollerL, &rollerR };
  for (int i = 0; i < 2; i++) {
    lv_obj_t* r = lv_roller_create(wheelScr);
    *rollers[i] = r;
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(r, 5);
    lv_obj_set_width(r, 150);
    lv_obj_align(r, LV_ALIGN_CENTER, i == 0 ? -78 : 78, 6);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_line_space(r, 8, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(r, lv_color_hex(COL_BORDER2), 0);
    lv_obj_set_style_border_width(r, 2, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_BORDER2), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(COL_ACCENT), LV_PART_SELECTED);
    lv_obj_add_event_cb(r, rollerEventCB, LV_EVENT_ALL, NULL);
  }
  lv_roller_set_selected(rollerR, 13, LV_ANIM_OFF);  // start right wheel at N

  // Bottom row, round-bezel safe: [backspace] [SEARCH] [clear]. Row center
  // sits 153 px below screen center; outer button corners stay inside the
  // 233 px glass radius.
  lv_obj_t* del = makeBtn(wheelScr, LV_SYMBOL_BACKSPACE, COL_BORDER2, deleteBtnCB);
  lv_obj_set_size(del, 72, 54);
  lv_obj_align(del, LV_ALIGN_BOTTOM_MID, -118, -52);
  lv_obj_t* dlbl = lv_obj_get_child(del, 0);
  lv_obj_set_style_text_color(dlbl, lv_color_hex(COL_TEXT), 0);

  searchBtn = makeBtn(wheelScr, "SEARCH", COL_ACCENT, searchBtnCB);
  lv_obj_set_size(searchBtn, 150, 54);
  lv_obj_align(searchBtn, LV_ALIGN_BOTTOM_MID, 0, -52);
  lv_obj_set_style_border_color(searchBtn, lv_color_hex(COL_BORDER),
                                LV_STATE_DISABLED);
  lv_obj_set_style_text_color(lv_obj_get_child(searchBtn, 0),
                              lv_color_hex(COL_DIM), LV_STATE_DISABLED);

  lv_obj_t* clr = makeBtn(wheelScr, LV_SYMBOL_CLOSE, COL_BORDER2, clearBtnCB);
  lv_obj_set_size(clr, 72, 54);
  lv_obj_align(clr, LV_ALIGN_BOTTOM_MID, 118, -52);
  lv_obj_t* clbl = lv_obj_get_child(clr, 0);
  lv_obj_set_style_text_color(clbl, lv_color_hex(COL_TEXT), 0);
}

/* ---- screen 2: word list ----------------------------------------------- */

static void listBackCB(lv_event_t* e) {
  (void)e;  // word untouched -> wheel resumes where it was
  updateWheel();
  lv_scr_load_anim(wheelScr, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

static void buildDefScreen(uint32_t index);  // fwd (row tap -> screen 3)

static void listRowCB(lv_event_t* e) {
  uint32_t index = (uint32_t)(intptr_t)lv_obj_get_user_data(
      lv_event_get_target(e));
  buildDefScreen(index);
  lv_scr_load_anim(defScr, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

// One word row: display form + sense count line, first-definition snippet
// under it. Exact match gets the accent treatment + pinned to the top.
static void addListRow(lv_obj_t* list, uint32_t index, bool exact) {
  DictEntry entry;
  if (!dictReadEntry(index, &entry) || entry.senseCount == 0) return;

  lv_obj_t* btn = lv_btn_create(list);
  lv_obj_set_width(btn, LV_PCT(100));
  lv_obj_set_height(btn, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(btn, 8, 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(exact ? COL_PANEL2 : COL_PANEL), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(exact ? COL_ACCENT : COL_BORDER), 0);
  lv_obj_set_user_data(btn, (void*)(intptr_t)index);
  lv_obj_add_event_cb(btn, listRowCB, LV_EVENT_CLICKED, NULL);

  lv_obj_t* name = lv_label_create(btn);
  char buf[DICT_DISPLAY_LEN + 32];
  snprintf(buf, sizeof(buf), "%s%s  -  %d sense%s",
           exact ? LV_SYMBOL_OK " " : "", entry.display,
           entry.senseCount, entry.senseCount == 1 ? "" : "s");
  lv_label_set_text(name, buf);
  lv_obj_set_width(name, LV_PCT(100));
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(name, lv_color_hex(exact ? COL_ACCENT : COL_TEXT), 0);
  lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* snip = lv_label_create(btn);
  // Single-line ellipsized first-definition snippet; truncate the copy too
  // so 50 rows never hold 50 full paragraphs in LVGL memory.
  snprintf(buf, sizeof(buf), "%.80s", entry.senses[0].def);
  lv_label_set_text(snip, buf);
  lv_obj_set_width(snip, LV_PCT(100));
  lv_label_set_long_mode(snip, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(snip, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(snip, lv_color_hex(COL_DIM), 0);
  lv_obj_align(snip, LV_ALIGN_TOP_LEFT, 0, 22);
}

static void buildListScreen() {
  if (listScr) lv_obj_del(listScr);  // rebuilt per search, never while active
  if (defScr) { lv_obj_del(defScr); defScr = NULL; }  // rows it linked to are gone
  listScr = makeScreen();

  uint32_t first = 0, count = 0;
  dictPrefixRange(word, &first, &count);
  int exact = dictExact(word);

  lv_obj_t* title = lv_label_create(listScr);
  char buf[DICT_KEY_LEN + 8];
  snprintf(buf, sizeof(buf), "%s...", word);
  lv_label_set_text(title, buf);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t* sub = lv_label_create(listScr);
  if (count == 0 && exact < 0)
    lv_label_set_text(sub, "not found");
  else
    lv_label_set_text_fmt(sub, "%u words", (unsigned)count);
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(COL_DIM), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 62);

  // Round-safe list body (same 350-wide footprint family as ui.cpp lists,
  // shortened for the header + back button).
  lv_obj_t* list = lv_obj_create(listScr);
  lv_obj_set_size(list, 350, 270);
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 6);
  lv_obj_set_style_radius(list, 24, 0);
  lv_obj_set_style_clip_corner(list, true, 0);
  lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(list, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_border_width(list, 1, 0);
  lv_obj_set_style_pad_all(list, 8, 0);
  lv_obj_set_style_pad_row(list, 6, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);

  // Pinned exact match first, then prefix matches in index order, LIST_ROWS
  // rows total.
  int shown = 0;
  if (exact >= 0) {
    addListRow(list, (uint32_t)exact, true);
    shown++;
  }
  for (uint32_t i = first; i < first + count && shown < LIST_ROWS; i++) {
    if (exact >= 0 && i == (uint32_t)exact) continue;  // already pinned
    addListRow(list, i, false);
    shown++;
  }

  uint32_t total = count;  // exact is inside the prefix range when present
  if (total > (uint32_t)shown) {
    lv_obj_t* foot = lv_label_create(list);
    lv_label_set_text_fmt(foot, "+%u more - keep spelling to narrow",
                          (unsigned)(total - (uint32_t)shown));
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(foot, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(foot, 8, 0);
  }

  makeBackBtn(listScr, listBackCB);
}

/* ---- screen 3: definition ---------------------------------------------- */

static void defBackCB(lv_event_t* e) {
  (void)e;  // listScr stays alive underneath -> straight back
  lv_scr_load_anim(listScr, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

static void buildDefScreen(uint32_t index) {
  if (defScr) lv_obj_del(defScr);  // rebuilt per word, never while active
  defScr = makeScreen();

  DictEntry entry;
  if (!dictReadEntry(index, &entry)) {  // can't happen after a listed row; be safe
    entry.display[0] = '?';
    entry.display[1] = '\0';
    entry.senseCount = 0;
  }

  lv_obj_t* title = lv_label_create(defScr);
  lv_label_set_text(title, entry.display);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(COL_ACCENT), 0);
  lv_obj_set_width(title, 340);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t* sub = lv_label_create(defScr);
  // senseCount caps at DICT_MAX_SENSES — giant merged entries show "16".
  lv_label_set_text_fmt(sub, "%d sense%s", entry.senseCount,
                        entry.senseCount == 1 ? "" : "s");
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(COL_DIM), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 62);

  lv_obj_t* body = lv_obj_create(defScr);
  lv_obj_set_size(body, 350, 270);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, 6);
  lv_obj_set_style_radius(body, 24, 0);
  lv_obj_set_style_clip_corner(body, true, 0);
  lv_obj_set_style_bg_color(body, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(body, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_border_width(body, 1, 0);
  lv_obj_set_style_pad_all(body, 10, 0);
  lv_obj_set_style_pad_row(body, 10, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);

  for (int i = 0; i < entry.senseCount; i++) {
    lv_obj_t* sense = lv_label_create(body);
    // "i.pos definition" — accent-recolored number/pos prefix.
    char buf[320];
    snprintf(buf, sizeof(buf), "#3EE8A0 %d.%s#  %s", i + 1,
             entry.senses[i].pos, entry.senses[i].def);
    lv_label_set_recolor(sense, true);
    lv_label_set_text(sense, buf);
    lv_obj_set_width(sense, LV_PCT(100));
    lv_label_set_long_mode(sense, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(sense, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sense, lv_color_hex(COL_TEXT), 0);
  }

  makeBackBtn(defScr, defBackCB);
}

/* ---- failure screen + entry points ------------------------------------- */

static void failBackCB(lv_event_t* e) {
  (void)e;
  hideDictScreen();
}

static void buildFailScreen() {
  failScr = makeScreen();
  lv_obj_t* msg = lv_label_create(failScr);
  lv_label_set_text(msg, "No dictionary on SD card");
  lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(msg, lv_color_hex(COL_TEXT), 0);
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -20);
  makeBackBtn(failScr, failBackCB);
}

void showDictScreen() {
  // Entry/exit animations mirror Focus, the swipe-down slot's previous
  // owner: slide in from the bottom, slide back out the top (exit now rides
  // the BOOT button, not a swipe). Transitions between the dict's own
  // screens stay FADE_ON like ui.cpp's sub-screens.
  if (!dictScreenActive()) prevScreen = lv_scr_act();
  if (!dictInit()) {  // idempotent; retried on every open until it lands
    if (!failScr) buildFailScreen();
    lv_scr_load_anim(failScr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
    return;
  }
  if (!wheelScr) buildWheelScreen();
  // Fresh entry: rollers back to their home letters (left A, right N —
  // user request); the word itself still survives close/reopen.
  lv_roller_set_selected(rollerL, 0, LV_ANIM_OFF);
  lv_roller_set_selected(rollerR, 13, LV_ANIM_OFF);
  updateWheel();
  lv_scr_load_anim(wheelScr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
}

void hideDictScreen() {
  if (!prevScreen) return;
  lv_scr_load_anim(prevScreen, LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, false);
}

bool dictScreenActive() {
  lv_obj_t* act = lv_scr_act();
  return act && (act == wheelScr || act == listScr ||
                 act == defScr || act == failScr);
}
