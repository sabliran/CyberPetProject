#pragma once
#include <lvgl.h>

// Dictionary app UI (phase 4): three screens — letter wheel -> word list ->
// definition — over the phase-3 dict API (dict.h). Self-contained module:
// builds its own LVGL screens lazily and never touches PetUI; phase 5 wires
// it into the pet's navigation. LVGL v8 APIs only (sim pin: v8.3.11).
//
// NOTE: docs/dict-ui-mockup.html is referenced as the approved visual spec
// but is absent from the repo — layout below follows the written behavioral
// spec + ui.cpp's styling idiom. Reconcile visuals when the mockup lands.

// Opens the dictionary (lazy dictInit on first use; a "No dictionary on SD
// card" screen with a back button if init fails). Remembers the screen that
// was active so hideDictScreen()/back can return to it.
void showDictScreen();

// Returns to the screen that was active before showDictScreen().
void hideDictScreen();

// True while any dictionary screen is the active screen (sim uses this to
// make the 'g' key a toggle; phase 5 will use it for gesture routing).
bool dictScreenActive();
