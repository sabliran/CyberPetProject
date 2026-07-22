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
// Walk frames come per view and direction: r = moving right (body-mirrored
// where the art is directional). walk = front view, back = walking away
// (depth zoom shrinking; no r — near-symmetric from behind), side = profile
// for mostly-horizontal glides. The firmware hides the eyes for back and
// side (no screen visible). The head is identical within each view.
#define SPRITE_WALK_SET(stage)             \
  extern const lv_img_dsc_t stage##_walk1; \
  extern const lv_img_dsc_t stage##_walk2; \
  extern const lv_img_dsc_t stage##_walk1r;\
  extern const lv_img_dsc_t stage##_walk2r;\
  extern const lv_img_dsc_t stage##_back1; \
  extern const lv_img_dsc_t stage##_back2; \
  extern const lv_img_dsc_t stage##_side1; \
  extern const lv_img_dsc_t stage##_side2; \
  extern const lv_img_dsc_t stage##_side1r;\
  extern const lv_img_dsc_t stage##_side2r;
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
