#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>

// Rendering modes
#define SYMBOL_RENDERING_MONOCHROME   0
#define SYMBOL_RENDERING_HIERARCHICAL 1
#define SYMBOL_RENDERING_PALETTE      2
#define SYMBOL_RENDERING_MULTICOLOR   3

// Animation types
#define SYMBOL_ANIM_NONE        0
#define SYMBOL_ANIM_BOUNCE      1
#define SYMBOL_ANIM_SCALE       2
#define SYMBOL_ANIM_PULSE       3
#define SYMBOL_ANIM_DRAW        4
#define SYMBOL_ANIM_WIGGLE      5
#define SYMBOL_ANIM_BREATHE     6
#define SYMBOL_ANIM_ROTATE      7
#define SYMBOL_ANIM_APPEAR      8
#define SYMBOL_ANIM_DISAPPEAR   9
#define SYMBOL_ANIM_REPLACE     10

// Layer modes (NSSymbolEffect only)
#define SYMBOL_LAYER_WHOLE        0
#define SYMBOL_LAYER_BY_LAYER     1
#define SYMBOL_LAYER_INDIVIDUALLY 2

// Returns a CGImageRef the caller owns and must CGImageRelease.
// For monochrome: returns white-on-transparent mask, sets *is_template_out = true.
// For other modes: returns pre-coloured image, sets *is_template_out = false.
// Returns NULL on failure or macOS < 11.
CGImageRef symbol_create(const char* name,
                         const char* weight,
                         const char* scale,
                         float       size,
                         uint8_t     rendering,
                         float*      palette_r,
                         float*      palette_g,
                         float*      palette_b,
                         float*      palette_a,
                         int         palette_count,
                         bool*       is_template_out);

// Opaque native-effect animation context (macOS 14+).
typedef struct symbol_effect_ctx symbol_effect_ctx;

// Start a native NSSymbolEffect animation. Returns NULL on macOS < 14 or failure.
symbol_effect_ctx* symbol_effect_start(const char* name,
                                       const char* weight,
                                       const char* scale,
                                       float       size,
                                       uint8_t     rendering,
                                       float*      palette_r,
                                       float*      palette_g,
                                       float*      palette_b,
                                       float*      palette_a,
                                       int         palette_count,
                                       uint8_t     anim_type,
                                       uint8_t     layer_mode,
                                       bool        repeat,
                                       float       speed);

// Stop and free an animation context.
void symbol_effect_stop(symbol_effect_ctx* ctx);

// Returns the latest captured frame. CGImageRef is owned by ctx — do NOT release.
// Returns NULL if no frame has been captured yet.
CGImageRef symbol_effect_current_frame(symbol_effect_ctx* ctx);
