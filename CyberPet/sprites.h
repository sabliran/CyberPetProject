#pragma once
#include <lvgl.h>

// Pixel-art body sprites, one per evolution stage, sized exactly to
// stageSize(stage) so no zoom is needed at rest. Bodies only — the animated
// eyes/expressions render as child objects on top (on Smol's black TV
// screen; gen_sprites.py pins the screen center at the ui.cpp eye anchor).
// Blob/creature/evolved are the same Smol art at 3x/4x/5x — Smol grows.
// ⚠ Pixel data is baked for LV_COLOR_DEPTH 16 + LV_COLOR_16_SWAP 1 (device).
#ifdef __cplusplus
extern "C" {
#endif
extern const lv_img_dsc_t sprite_egg;             // 80x80 (no walk frames — unhatched)
// Walk frames come per direction: the art's body sway trails the movement,
// so plain walk1/walk2 are for LEFTWARD glides and the r (body-mirrored)
// variants for rightward. back1/back2 (+r) show the pet from behind while
// a glide walks it away from the viewer (depth zoom shrinking) — the
// firmware hides the eyes then. The head is identical within each view.
#define SPRITE_WALK_SET(stage)             \
  extern const lv_img_dsc_t stage##_walk1; \
  extern const lv_img_dsc_t stage##_walk2; \
  extern const lv_img_dsc_t stage##_walk1r;\
  extern const lv_img_dsc_t stage##_walk2r;\
  extern const lv_img_dsc_t stage##_back1; \
  extern const lv_img_dsc_t stage##_back2; \
  extern const lv_img_dsc_t stage##_back1r;\
  extern const lv_img_dsc_t stage##_back2r;
extern const lv_img_dsc_t sprite_blob;            // 165x165, idle pose
SPRITE_WALK_SET(sprite_blob)
extern const lv_img_dsc_t sprite_creature;        // 205x205
SPRITE_WALK_SET(sprite_creature)
extern const lv_img_dsc_t sprite_evolved;         // 245x245
SPRITE_WALK_SET(sprite_evolved)
#undef SPRITE_WALK_SET
#ifdef __cplusplus
}
#endif
