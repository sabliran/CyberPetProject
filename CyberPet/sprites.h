#pragma once
#include <lvgl.h>

// Pixel-art body sprites, one per evolution stage, sized exactly to
// stageSize(stage) so no zoom is needed at rest. Bodies only — the animated
// eyes/expressions render as child objects on top.
// ⚠ Pixel data is baked for LV_COLOR_DEPTH 16 + LV_COLOR_16_SWAP 1 (device).
#ifdef __cplusplus
extern "C" {
#endif
extern const lv_img_dsc_t sprite_egg;             // 80x80 (no walk frames — no legs)
extern const lv_img_dsc_t sprite_blob;            // 110x110, both feet planted
extern const lv_img_dsc_t sprite_blob_walk1;      //   left foot lifted
extern const lv_img_dsc_t sprite_blob_walk2;      //   right foot lifted
extern const lv_img_dsc_t sprite_creature;        // 140x140
extern const lv_img_dsc_t sprite_creature_walk1;
extern const lv_img_dsc_t sprite_creature_walk2;
extern const lv_img_dsc_t sprite_evolved;         // 170x170
extern const lv_img_dsc_t sprite_evolved_walk1;
extern const lv_img_dsc_t sprite_evolved_walk2;
#ifdef __cplusplus
}
#endif
