#include "text.h"
#include "bar_manager.h"
#include <string.h>

// ---------------------------------------------------------------------------
// SF Symbols — CTRunDelegate support
// ---------------------------------------------------------------------------

struct symbol_run_data {
  CGImageRef image;
  CGFloat    width;
  CGFloat    ascent;
  CGFloat    descent;
  bool       is_template;   // true → mask-tint; false → draw directly
  symbol_effect_ctx* effect; // non-NULL when a native animation is active
};

static CGFloat sym_get_ascent (void* r) { return ((struct symbol_run_data*)r)->ascent;  }
static CGFloat sym_get_descent(void* r) { return ((struct symbol_run_data*)r)->descent; }
static CGFloat sym_get_width  (void* r) { return ((struct symbol_run_data*)r)->width;   }
static void    sym_dealloc    (void* r) {
  struct symbol_run_data* d = r;
  if (d->image) CGImageRelease(d->image);
  // effect context lifetime is managed by struct text, not by the run data
  free(d);
}

// Returns true when ch and the following bytes form a valid SF Symbol token
// matching [a-zA-Z][a-zA-Z0-9._-]* terminated by '>'.
// On success, writes a NUL-terminated symbol name into out_name (max_len bytes)
// and sets *end to point at the character after '>'.
static bool parse_symbol_token(const char* ch, char* out_name, size_t max_len,
                               const char** end) {
  if (*ch != '<') return false;
  const char* p = ch + 1;
  if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z')) return false;
  size_t len = 0;
  while (*p && *p != '>') {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
    if (len + 1 < max_len) out_name[len++] = c;
    p++;
  }
  if (*p != '>') return false;
  out_name[len] = '\0';
  *end = p + 1;
  return true;
}

// Returns true if the string contains at least one valid <symbol> token.
static bool text_has_symbols(const char* str) {
  if (!str) return false;
  char name[128];
  const char* p = str;
  while (*p) {
    if (*p == '<') {
      const char* end;
      if (parse_symbol_token(p, name, sizeof(name), &end)) return true;
    }
    p++;
  }
  return false;
}

// Post-draw helper: render any CTRunDelegate symbol runs into the context.
static void text_draw_symbol_runs(CTLineRef line, CGContextRef ctx,
                                  CGFloat ox, CGFloat oy,
                                  struct color* col,
                                  float anim_scale, float anim_alpha) {
  if (!line) return;
  CFArrayRef runs = CTLineGetGlyphRuns(line);
  CFIndex run_count = CFArrayGetCount(runs);
  for (CFIndex i = 0; i < run_count; i++) {
    CTRunRef run = CFArrayGetValueAtIndex(runs, i);
    CFDictionaryRef attrs = CTRunGetAttributes(run);
    CTRunDelegateRef del = CFDictionaryGetValue(attrs, kCTRunDelegateAttributeName);
    if (!del) continue;
    struct symbol_run_data* d = CTRunDelegateGetRefCon(del);
    if (!d) continue;

    CGImageRef image = d->image;
    bool is_template = d->is_template;
    if (d->effect) {
      CGImageRef live = symbol_effect_current_frame(d->effect);
      if (live) { image = live; is_template = false; }
    }
    if (!image) continue;

    CGPoint pos[1];
    CTRunGetPositions(run, CFRangeMake(0, 1), pos);
    CGFloat sym_w = d->width  * anim_scale;
    CGFloat sym_h = (d->ascent + d->descent) * anim_scale;
    CGFloat x = ox + pos[0].x - (sym_w - d->width) * 0.5f;
    CGFloat y = oy + pos[0].y - d->descent * anim_scale;
    CGRect rect = CGRectMake(x, y, sym_w, sym_h);

    CGContextSaveGState(ctx);
    if (is_template) {
      CGContextSetAlpha(ctx, anim_alpha);
      CGContextClipToMask(ctx, rect, image);
      CGContextSetRGBFillColor(ctx, col->r, col->g, col->b, col->a);
      CGContextFillRect(ctx, rect);
    } else {
      CGContextSetAlpha(ctx, anim_alpha);
      CGContextDrawImage(ctx, rect, image);
    }
    CGContextRestoreGState(ctx);
  }
}

static void text_calculate_truncated_width(struct text* text, CFDictionaryRef attributes) {
  if (text->max_chars > 0) {
    uint32_t len = strlen(text->string) + 4;
    char buffer[len];
    memset(buffer, 0, len);

    char* read = text->string;
    char* write = buffer;
    uint32_t counter = 0;
    while (*read) {
      if ((*read & 0xC0) != 0x80) counter++; 
      if (counter > text->max_chars) {
        break;
      }
      *write++ = *read++;
    }

    CFStringRef string = CFStringCreateWithCString(NULL,
                                                   buffer,
                                                   kCFStringEncodingUTF8);

    if (string) {
      CFAttributedStringRef attr_string = CFAttributedStringCreate(NULL,
                                                                   string,
                                                                   attributes);

      CTLineRef line = CTLineCreateWithAttributedString(attr_string);

      CGRect bounds = CTLineGetBoundsWithOptions(line,
                                              kCTLineBoundsUseGlyphPathBounds);
      text->width = (uint32_t)(bounds.size.width + 1.5);
      CFRelease(attr_string);
      CFRelease(line);
      CFRelease(string);
    }
  }
}

static void text_prepare_line(struct text* text) {
  const void *keys[] = { kCTFontAttributeName,
                         kCTForegroundColorFromContextAttributeName };

  if (text->font.font_changed) {
    font_create_ctfont(&text->font);
    text->font.font_changed = false;
  }
  const void *values[] = { text->font.ct_font, kCFBooleanTrue };
  CFDictionaryRef attributes = CFDictionaryCreate(NULL,
                                                  keys,
                                                  values,
                                                  array_count(keys),
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);

  // Fast path: no symbol tokens in string
  if (!text_has_symbols(text->string)) {
    CFStringRef string = CFStringCreateWithCString(NULL,
                                                   text->string,
                                                   kCFStringEncodingUTF8);

    if (!string) string = CFStringCreateWithCString(NULL,
                                            "Warning: Malformed UTF-8 string",
                                            kCFStringEncodingUTF8             );

    CFAttributedStringRef attr_string = CFAttributedStringCreate(NULL,
                                                                 string,
                                                                 attributes);
    text->line.line = CTLineCreateWithAttributedString(attr_string);
    CTLineGetTypographicBounds(text->line.line,
                               &text->line.ascent,
                               &text->line.descent,
                               NULL                );
    text->bounds = CTLineGetBoundsWithOptions(text->line.line,
                                              kCTLineBoundsUseGlyphPathBounds);
    text->bounds.size.width  = (uint32_t)(text->bounds.size.width  + 1.5);
    text->bounds.size.height = (uint32_t)(text->bounds.size.height + 1.5);
    text->bounds.origin.x    = (int32_t) (text->bounds.origin.x   + 0.5);
    text->bounds.origin.y    = (int32_t) (text->bounds.origin.y   + 0.5);
    text->width = text->bounds.size.width;
    CFRelease(string);
    CFRelease(attr_string);
    text_calculate_truncated_width(text, attributes);
    CFRelease(attributes);
    return;
  }

  // Symbol path: build a mutable attributed string segment by segment.
  // Get font metrics for slot sizing.
  CGFloat font_ascent  = 0.0;
  CGFloat font_descent = 0.0;
  CGFloat font_size    = text->font.size;
  if (text->font.ct_font) {
    font_ascent  = CTFontGetAscent(text->font.ct_font);
    font_descent = CTFontGetDescent(text->font.ct_font);
  } else {
    font_ascent  = font_size * 0.75f;
    font_descent = font_size * 0.25f;
  }

  CTRunDelegateCallbacks sym_callbacks = {
    .version    = kCTRunDelegateVersion1,
    .dealloc    = sym_dealloc,
    .getAscent  = sym_get_ascent,
    .getDescent = sym_get_descent,
    .getWidth   = sym_get_width,
  };

  CFMutableAttributedStringRef mattr =
    CFAttributedStringCreateMutable(kCFAllocatorDefault, 0);

  const char* p = text->string;
  // Accumulate plain text bytes before flushing as a run
  char plain_buf[4096];
  size_t plain_len = 0;

  // Flush accumulated plain text as a styled run
  #define FLUSH_PLAIN() do { \
    if (plain_len > 0) { \
      plain_buf[plain_len] = '\0'; \
      CFStringRef s = CFStringCreateWithCString(NULL, plain_buf, kCFStringEncodingUTF8); \
      if (s) { \
        CFAttributedStringRef a = CFAttributedStringCreate(NULL, s, attributes); \
        CFAttributedStringReplaceAttributedString(mattr, \
          CFRangeMake(CFAttributedStringGetLength(mattr), 0), a); \
        CFRelease(a); \
        CFRelease(s); \
      } \
      plain_len = 0; \
    } \
  } while (0)

  while (*p) {
    if (*p == '<') {
      char sym_name[128];
      const char* end;
      if (parse_symbol_token(p, sym_name, sizeof(sym_name), &end)) {
        FLUSH_PLAIN();

        // Create the symbol image.
        // When a native effect context is active, d->image is left NULL and
        // live frames are fetched from d->effect in text_draw_symbol_runs().
        // This avoids retaining/releasing the frame owned by the effect ctx.
        bool is_template = true;
        CGImageRef img = NULL;
        bool has_effect = (text->symbol_effect != NULL);

        if (!has_effect) {
          img = symbol_create(sym_name,
                              text->symbol_weight,
                              text->symbol_scale,
                              font_size,
                              text->symbol_rendering,
                              text->symbol_palette_r,
                              text->symbol_palette_g,
                              text->symbol_palette_b,
                              text->symbol_palette_a,
                              text->symbol_palette_count,
                              &is_template);
        } else {
          is_template = false;
        }

        if (img || has_effect) {
          float sym_display_h = font_ascent + font_descent;
          float sym_display_w = sym_display_h; // default square slot
          if (img) {
            float img_w = (float)CGImageGetWidth(img);
            float img_h = (float)CGImageGetHeight(img);
            float aspect = img_w / (img_h > 0 ? img_h : 1.f);
            sym_display_w = aspect * sym_display_h;
            if (sym_display_w < 1.f) sym_display_w = sym_display_h;
          }

          struct symbol_run_data* data = malloc(sizeof(struct symbol_run_data));
          data->image       = img;       // NULL when effect drives live frames
          data->width       = sym_display_w;
          data->ascent      = font_ascent;
          data->descent     = font_descent;
          data->is_template = is_template;
          data->effect      = text->symbol_effect;

          CTRunDelegateRef delegate = CTRunDelegateCreate(&sym_callbacks, data);

          // U+FFFC OBJECT REPLACEMENT CHARACTER (UTF-8: 0xEF 0xBF 0xBC)
          static const char orc_utf8[] = "\xEF\xBF\xBC";
          CFStringRef orc = CFStringCreateWithCString(NULL, orc_utf8, kCFStringEncodingUTF8);
          if (orc) {
            CFAttributedStringRef a = CFAttributedStringCreate(NULL, orc, attributes);
            CFMutableAttributedStringRef ma = CFAttributedStringCreateMutableCopy(NULL, 0, a);
            CFIndex pos_in_line = CFAttributedStringGetLength(mattr);
            CFAttributedStringReplaceAttributedString(mattr,
              CFRangeMake(pos_in_line, 0), ma);
            CFAttributedStringSetAttribute(mattr,
              CFRangeMake(pos_in_line, 1),
              kCTRunDelegateAttributeName,
              delegate);
            CFRelease(ma);
            CFRelease(a);
            CFRelease(orc);
          }
          CFRelease(delegate);
        }
        p = end;
        continue;
      }
    }
    // Accumulate plain character (may be multi-byte UTF-8)
    if (plain_len < sizeof(plain_buf) - 5) {
      plain_buf[plain_len++] = *p;
    }
    p++;
  }
  FLUSH_PLAIN();
  #undef FLUSH_PLAIN

  text->line.line = CTLineCreateWithAttributedString(mattr);
  CFRelease(mattr);

  CTLineGetTypographicBounds(text->line.line,
                             &text->line.ascent,
                             &text->line.descent,
                             NULL                );

  text->bounds = CTLineGetBoundsWithOptions(text->line.line,
                                            kCTLineBoundsUseGlyphPathBounds);

  text->bounds.size.width  = (uint32_t)(text->bounds.size.width  + 1.5);
  text->bounds.size.height = (uint32_t)(text->bounds.size.height + 1.5);
  text->bounds.origin.x    = (int32_t) (text->bounds.origin.x   + 0.5);
  text->bounds.origin.y    = (int32_t) (text->bounds.origin.y   + 0.5);

  text->width = text->bounds.size.width;
  text_calculate_truncated_width(text, attributes);
  CFRelease(attributes);
}

static void text_destroy_line(struct text* text) {
  if (text->line.line) CFRelease(text->line.line);
  text->line.line = NULL;
}

bool text_set_max_chars(struct text* text, uint32_t max_chars) {
  if (text->max_chars == max_chars) return false;
  text->max_chars = max_chars;
  if (strlen(text->string) > text->max_chars) {
    text_set_string(text, text->string, true);
  }
  return strlen(text->string) > text->max_chars;
}

bool text_set_string(struct text* text, char* string, bool forced) {
  if (!string) return false;
  if (!forced && text->string && strcmp(text->string, string) == 0) { 
    if (!(string == text->string)) free(string);
    return false; 
  }
  if (text->line.line) text_destroy_line(text);
  if (string != text->string && text->string) free(text->string);
  text->string = string;
  text_prepare_line(text);
  return true;
}

void text_copy(struct text* text, struct text* source) {
  font_set_family(&text->font, string_copy(source->font.family), true);
  font_set_style(&text->font, string_copy(source->font.style), true);
  font_set_size(&text->font, source->font.size);
  text_set_string(text, string_copy(source->string), true);
}

bool text_set_font(struct text* text, char* font_string, bool forced) {
  bool changed = font_set(&text->font, font_string, forced);
  return changed;
}

void text_init(struct text* text) {
  text->drawing = true;
  text->highlight = false;
  text->has_const_width = false;
  text->custom_width = 0;
  text->padding_left = 0;
  text->padding_right = 0;
  text->y_offset = 0;
  text->max_chars = 0;
  text->align = POSITION_LEFT;
  text->scroll = 0.f;
  text->scroll_duration = 100;

  text->string = string_copy("");
  text_set_string(text, text->string, false);
  shadow_init(&text->shadow);
  background_init(&text->background);
  font_init(&text->font);

  color_init(&text->color, 0xffffffff);
  color_init(&text->highlight_color, 0xff000000);

  text->symbol_weight        = NULL;
  text->symbol_scale         = NULL;
  text->symbol_rendering     = SYMBOL_RENDERING_MONOCHROME;
  text->symbol_palette_count = 0;

  text->symbol_anim_type   = SYMBOL_ANIM_NONE;
  text->symbol_anim_layer  = SYMBOL_LAYER_WHOLE;
  text->symbol_anim_repeat = false;
  text->symbol_anim_speed  = 1.f;
  text->symbol_anim_scale  = 1.f;
  text->symbol_anim_alpha  = 1.f;
  text->symbol_effect      = NULL;
}

static bool text_set_color(struct text* text, uint32_t color) {
  bool changed = color_set_hex(&text->color, color);
  if (changed
      && text->symbol_rendering == SYMBOL_RENDERING_HIERARCHICAL
      && text->string && strchr(text->string, '<'))
    text_set_string(text, text->string, true);
  return changed;
}

static bool text_set_highlight_color(struct text* text, uint32_t color) {
  return color_set_hex(&text->highlight_color, color);
}

static bool text_set_padding_left(struct text* text, int padding) {
  if (text->padding_left == padding) return false;
  text->padding_left = padding;
  return true;
}

static bool text_set_padding_right(struct text* text, int padding) {
  if (text->padding_right == padding) return false;
  text->padding_right = padding;
  return true;
}

static bool text_set_yoffset(struct text* text, int offset) {
  if (text->y_offset == offset) return false;
  text->y_offset = offset;
  return true;
}

static bool text_set_scroll_duration(struct text* text, int duration) {
  if (duration < 0) return false;
  text->scroll_duration = duration;
  return false;
}

static bool text_set_width(struct text* text, int width) {
  if (width < 0) {
    bool prev = text->has_const_width;
    text->has_const_width = false;
    return prev != text->has_const_width;
  }

  if (text->custom_width == width && text->has_const_width) return false;
  text->custom_width = width;
  text->has_const_width = true;
  return true;
}

// Float-typed setters (used with ANIMATE_FLOAT / direct animation_setup + as_float)
static bool symbol_set_anim_scale(struct text* text, float value) {
  if (text->symbol_anim_scale == value) return false;
  text->symbol_anim_scale = value;
  return true;
}

static bool symbol_set_anim_alpha(struct text* text, float value) {
  if (text->symbol_anim_alpha == value) return false;
  text->symbol_anim_alpha = value;
  return true;
}

void text_clear_pointers(struct text* text) {
  text->string        = NULL;
  text->line.line     = NULL;
  text->symbol_weight = NULL;
  text->symbol_scale  = NULL;
  text->symbol_effect = NULL;
  background_clear_pointers(&text->background);
  font_clear_pointers(&text->font);
}

uint32_t text_get_length(struct text* text, bool override) {
  if (!text->drawing) return 0;

  if (text->font.font_changed) {
    text_set_string(text, text->string, true);
  }

  int len = text->width + text->padding_left + text->padding_right;
  if ((!text->has_const_width || override)
      && text->background.enabled
      && text->background.image.enabled) {
    CGSize image_size = image_get_size(&text->background.image);
    if (image_size.width > len) {
      return image_size.width;
    }
  }

  if (text->has_const_width && !override) return text->custom_width;
  return (len < 0 ? 0 : len);
}

uint32_t text_get_height(struct text* text) {
  return text->drawing ? text->bounds.size.height : 0;
}

void text_destroy(struct text* text) {
  if (text->symbol_effect) {
    symbol_effect_stop(text->symbol_effect);
    text->symbol_effect = NULL;
  }
  if (text->symbol_weight) free(text->symbol_weight);
  if (text->symbol_scale)  free(text->symbol_scale);

  background_destroy(&text->background);
  font_destroy(&text->font);

  if (text->string) free(text->string);
  text_destroy_line(text);
  text_clear_pointers(text);
}

void text_calculate_bounds(struct text* text, uint32_t x, uint32_t y) {
  if (text->align == POSITION_CENTER && text->has_const_width)
    text->bounds.origin.x = (int)x + ((int)text->custom_width
                                 - (int)text_get_length(text, true)) / 2;
  else if (text->align == POSITION_RIGHT && text->has_const_width)
    text->bounds.origin.x = (int)x + (int)text->custom_width
                            - (int)text_get_length(text, true);
  else
    text->bounds.origin.x = x;

  text->bounds.origin.y =(uint32_t)(y - ((text->line.ascent
                                          - text->line.descent) / 2));

  if (text->background.enabled) {
    uint32_t height = text->background.overrides_height
                      ? text->background.bounds.size.height
                      : text->bounds.size.height;

    background_calculate_bounds(&text->background,
                                x,
                                y,
                                text_get_length(text, false),
                                height                       );
  }
}

// Find the first symbol name in text->string, write to out_name.
static bool text_first_symbol_name(struct text* text, char* out_name, size_t max_len) {
  if (!text->string) return false;
  const char* p = text->string;
  while (*p) {
    if (*p == '<') {
      const char* end;
      if (parse_symbol_token(p, out_name, max_len, &end)) return true;
    }
    p++;
  }
  return false;
}

// Helper to queue a float animation directly (avoids ANIMATE_FLOAT's cancel-previous side effect).
// Chaining is automatic: animator_add detects same (target, func) and sets waiting=true.
static void queue_float_animation(void* target,
                                  bool (*func)(void*, float),
                                  float from, float to,
                                  uint32_t duration_frames,
                                  char interp) {
  struct animation* a = animation_create();
  animation_setup(a, target,
                  (bool (*)(void*, int))(void*)func,
                  *(int*)&from, *(int*)&to,
                  duration_frames, interp);
  a->as_float = true;
  animator_add(&g_bar_manager.animator, a);
}

static void text_queue_transform_animation(struct text* text, uint8_t anim_type) {
  switch (anim_type) {
    case SYMBOL_ANIM_BOUNCE:
      // Phase 1: scale up quickly (circ easing)
      queue_float_animation(text, symbol_set_anim_scale,
                            text->symbol_anim_scale, 1.35f,
                            8, INTERP_FUNCTION_CIRC);
      // Phase 2: spring back to normal (circ easing, chained automatically)
      queue_float_animation(text, symbol_set_anim_scale,
                            1.35f, 1.0f,
                            12, INTERP_FUNCTION_CIRC);
      break;
    case SYMBOL_ANIM_SCALE:
      queue_float_animation(text, symbol_set_anim_scale,
                            text->symbol_anim_scale, 1.4f,
                            15, INTERP_FUNCTION_CIRC);
      if (!text->symbol_anim_repeat)
        queue_float_animation(text, symbol_set_anim_scale,
                              1.4f, 1.0f,
                              15, INTERP_FUNCTION_CIRC);
      break;
    case SYMBOL_ANIM_PULSE:
      queue_float_animation(text, symbol_set_anim_alpha,
                            text->symbol_anim_alpha, 0.2f,
                            20, INTERP_FUNCTION_SIN);
      queue_float_animation(text, symbol_set_anim_alpha,
                            0.2f, 1.0f,
                            20, INTERP_FUNCTION_SIN);
      break;
    default:
      break;
  }
}

static bool text_set_symbol_animate(struct text* text, uint8_t anim_type) {
  // Stop existing native effect
  if (text->symbol_effect) {
    symbol_effect_stop(text->symbol_effect);
    text->symbol_effect = NULL;
  }
  text->symbol_anim_type  = anim_type;
  text->symbol_anim_scale = 1.f;
  text->symbol_anim_alpha = 1.f;

  if (anim_type == SYMBOL_ANIM_NONE) return true;

  if (anim_type <= SYMBOL_ANIM_PULSE) {
    text_queue_transform_animation(text, anim_type);
  } else {
    // Native NSSymbolEffect (macOS 14+)
    char sym_name[128];
    if (!text_first_symbol_name(text, sym_name, sizeof(sym_name))) return false;
    text->symbol_effect = symbol_effect_start(sym_name,
                                              text->symbol_weight,
                                              text->symbol_scale,
                                              text->font.size,
                                              text->symbol_rendering,
                                              text->symbol_palette_r,
                                              text->symbol_palette_g,
                                              text->symbol_palette_b,
                                              text->symbol_palette_a,
                                              text->symbol_palette_count,
                                              anim_type,
                                              text->symbol_anim_layer,
                                              text->symbol_anim_repeat,
                                              text->symbol_anim_speed);
    // Re-prepare so run data gets the effect_ctx pointer
    text_set_string(text, text->string, true);
  }
  return true;
}

static bool text_set_symbol_property(struct text* text, FILE* rsp,
                                     struct token entry, char* message);

bool text_set_scroll(struct text* text, float scroll) {
  if (text->scroll == scroll) return false;
  text->scroll = scroll;
  return true;
}

bool text_animate_scroll(struct text* text) {
  if (text->max_chars == 0) return false;
  if (text->scroll != 0) return false;
  if (text->has_const_width && text->custom_width < text->width) return false;
  if (text->width == 0 || text->width == text->bounds.size.width) return false;

  g_bar_manager.animator.duration = text->scroll_duration
                                    * (text->bounds.size.width / text->width);
  g_bar_manager.animator.interp_function = INTERP_FUNCTION_LINEAR;

  bool needs_refresh = false;
  ANIMATE_FLOAT(text_set_scroll,
                text,
                text->scroll,
                max(text->bounds.size.width, 0));

  struct animation* animation = animation_create();
  float initial_value = text->scroll;
  float final_value = -max(text->width, 0);

  animation_setup(animation,
                  (void*)text,
                  (bool (*)(void*, int))text_set_scroll,
                  *(int*)&initial_value,
                  *(int*)&final_value,
                  0,
                  INTERP_FUNCTION_LINEAR );
  animation->as_float = true;
  animator_add(&g_bar_manager.animator, animation);

  g_bar_manager.animator.duration = text->scroll_duration;
  ANIMATE_FLOAT(text_set_scroll, text, text->scroll, 0);

  g_bar_manager.animator.duration = 0;
  g_bar_manager.animator.interp_function = '\0';

  return needs_refresh;
}

void text_draw(struct text* text, CGContextRef context) {
  if (!text->drawing) return;
  if (text->background.enabled)
    background_draw(&text->background, context);

  CGContextSaveGState(context);
  if (text->max_chars > 0) {
    CGMutablePathRef path = CGPathCreateMutable();
    CGRect bounds = text->bounds;
    bounds.size.width = text->width;
    bounds.origin.x += text->padding_left;
    bounds.origin.y = -9999.f;
    bounds.size.height = 2.f*9999.f;

    CGPathAddRect(path, NULL, bounds);

    CGContextAddPath(context, path);
    CGContextClip(context);
    CFRelease(path);
  }

  if (text->shadow.enabled) {
    CGContextSetRGBFillColor(context,
                             text->shadow.color.r,
                             text->shadow.color.g,
                             text->shadow.color.b,
                             text->shadow.color.a );

    CGRect shadow_bounds = shadow_get_bounds(&text->shadow, text->bounds);
    CGFloat sox = shadow_bounds.origin.x + text->padding_left;
    CGFloat soy = shadow_bounds.origin.y + text->y_offset;
    CGContextSetTextPosition(context, sox, soy);
    CTLineDraw(text->line.line, context);
    struct color shadow_color = { .r = text->shadow.color.r,
                                  .g = text->shadow.color.g,
                                  .b = text->shadow.color.b,
                                  .a = text->shadow.color.a };
    text_draw_symbol_runs(text->line.line, context, sox, soy,
                          &shadow_color,
                          text->symbol_anim_scale, text->symbol_anim_alpha);
  }

  struct color color = text->highlight ? text->highlight_color : text->color;
  CGContextSetRGBFillColor(context, color.r, color.g, color.b, color.a);

  CGFloat ox = text->bounds.origin.x + text->padding_left - text->scroll;
  CGFloat oy = text->bounds.origin.y + text->y_offset;
  CGContextSetTextPosition(context, ox, oy);
  CTLineDraw(text->line.line, context);
  text_draw_symbol_runs(text->line.line, context, ox, oy,
                        &color,
                        text->symbol_anim_scale, text->symbol_anim_alpha);
  CGContextRestoreGState(context);
}

void text_serialize(struct text* text, char* indent, FILE* rsp) {
  char align[32] = { 0 };
  switch (text->align) {
    case POSITION_LEFT:
      snprintf(align, 32, "left");
      break;
    case POSITION_RIGHT:
      snprintf(align, 32, "right");
      break;
    case POSITION_CENTER:
      snprintf(align, 32, "center");
      break;
    case POSITION_BOTTOM:
      snprintf(align, 32, "bottom");
      break;
    case POSITION_TOP:
      snprintf(align, 32, "top");
      break;
    default:
      snprintf(align, 32, "invalid");
      break;
  }

  fprintf(rsp, "%s\"value\": \"%s\",\n"
               "%s\"drawing\": \"%s\",\n"
               "%s\"highlight\": \"%s\",\n"
               "%s\"color\": \"0x%x\",\n"
               "%s\"highlight_color\": \"0x%x\",\n"
               "%s\"padding_left\": %d,\n"
               "%s\"padding_right\": %d,\n"
               "%s\"y_offset\": %d,\n"
               "%s\"font\": \"%s:%s:%.2f\",\n"
               "%s\"width\": %d,\n"
               "%s\"scroll_duration\": %d,\n"
               "%s\"align\": \"%s\",\n"
               "%s\"background\": {\n",
               indent, text->string,
               indent, format_bool(text->drawing),
               indent, format_bool(text->highlight),
               indent, text->color.hex,
               indent, text->highlight_color.hex,
               indent, text->padding_left,
               indent, text->padding_right,
               indent, text->y_offset,
               indent, text->font.family, text->font.style, text->font.size,
               indent, text->custom_width,
               indent, text->scroll_duration,
               indent, align, indent                                        );

  char deeper_indent[strlen(indent) + 2];
  snprintf(deeper_indent, strlen(indent) + 2, "%s\t", indent);
  background_serialize(&text->background, deeper_indent, rsp, true);

  fprintf(rsp, "\n%s},\n%s\"shadow\": {\n", indent, indent);
  shadow_serialize(&text->shadow, deeper_indent, rsp);
  fprintf(rsp, "\n%s}", indent);
}

static bool text_set_symbol_property(struct text* text, FILE* rsp,
                                     struct token entry, char* message) {
  if (token_equals(entry, PROPERTY_SYMBOL_WEIGHT)) {
    if (text->symbol_weight) free(text->symbol_weight);
    text->symbol_weight = token_to_string(get_token(&message));
    return text_set_string(text, text->string, true);

  } else if (token_equals(entry, PROPERTY_SYMBOL_SCALE)) {
    if (text->symbol_scale) free(text->symbol_scale);
    text->symbol_scale = token_to_string(get_token(&message));
    return text_set_string(text, text->string, true);

  } else if (token_equals(entry, PROPERTY_SYMBOL_RENDERING)) {
    char* val = token_to_string(get_token(&message));
    uint8_t prev = text->symbol_rendering;
    if      (strcmp(val, "hierarchical") == 0) text->symbol_rendering = SYMBOL_RENDERING_HIERARCHICAL;
    else if (strcmp(val, "palette")      == 0) text->symbol_rendering = SYMBOL_RENDERING_PALETTE;
    else if (strcmp(val, "multicolor")   == 0) text->symbol_rendering = SYMBOL_RENDERING_MULTICOLOR;
    else                                       text->symbol_rendering = SYMBOL_RENDERING_MONOCHROME;
    free(val);
    if (text->symbol_rendering == prev) return false;
    return text_set_string(text, text->string, true);

  } else if (token_equals(entry, PROPERTY_SYMBOL_PALETTE)) {
    // Parse comma-separated 0xAARRGGBB hex values, up to 3
    char* val = token_to_string(get_token(&message));
    int count = 0;
    char* tok = strtok(val, ",");
    while (tok && count < 3) {
      while (*tok == ' ') tok++;
      uint32_t hex = (uint32_t)strtoul(tok, NULL, 16);
      text->symbol_palette_r[count] = ((hex >> 16) & 0xFF) / 255.f;
      text->symbol_palette_g[count] = ((hex >>  8) & 0xFF) / 255.f;
      text->symbol_palette_b[count] = ((hex      ) & 0xFF) / 255.f;
      text->symbol_palette_a[count] = ((hex >> 24) & 0xFF) / 255.f;
      count++;
      tok = strtok(NULL, ",");
    }
    text->symbol_palette_count = count;
    free(val);
    return text_set_string(text, text->string, true);

  } else if (token_equals(entry, PROPERTY_SYMBOL_ANIMATE)) {
    char* val = token_to_string(get_token(&message));
    uint8_t anim = SYMBOL_ANIM_NONE;
    if      (strcmp(val, "bounce")    == 0) anim = SYMBOL_ANIM_BOUNCE;
    else if (strcmp(val, "scale")     == 0) anim = SYMBOL_ANIM_SCALE;
    else if (strcmp(val, "pulse")     == 0) anim = SYMBOL_ANIM_PULSE;
    else if (strcmp(val, "draw")      == 0) anim = SYMBOL_ANIM_DRAW;
    else if (strcmp(val, "wiggle")    == 0) anim = SYMBOL_ANIM_WIGGLE;
    else if (strcmp(val, "breathe")   == 0) anim = SYMBOL_ANIM_BREATHE;
    else if (strcmp(val, "rotate")    == 0) anim = SYMBOL_ANIM_ROTATE;
    else if (strcmp(val, "appear")    == 0) anim = SYMBOL_ANIM_APPEAR;
    else if (strcmp(val, "disappear") == 0) anim = SYMBOL_ANIM_DISAPPEAR;
    else if (strcmp(val, "replace")   == 0) anim = SYMBOL_ANIM_REPLACE;
    free(val);
    return text_set_symbol_animate(text, anim);

  } else if (token_equals(entry, PROPERTY_SYMBOL_ANIM_LAYER)) {
    char* val = token_to_string(get_token(&message));
    uint8_t prev = text->symbol_anim_layer;
    if      (strcmp(val, "by_layer")     == 0) text->symbol_anim_layer = SYMBOL_LAYER_BY_LAYER;
    else if (strcmp(val, "individually") == 0) text->symbol_anim_layer = SYMBOL_LAYER_INDIVIDUALLY;
    else                                       text->symbol_anim_layer = SYMBOL_LAYER_WHOLE;
    free(val);
    return text->symbol_anim_layer != prev;

  } else if (token_equals(entry, PROPERTY_SYMBOL_ANIM_REPEAT)) {
    bool prev = text->symbol_anim_repeat;
    text->symbol_anim_repeat = evaluate_boolean_state(get_token(&message),
                                                      text->symbol_anim_repeat);
    return text->symbol_anim_repeat != prev;

  } else if (token_equals(entry, PROPERTY_SYMBOL_ANIM_SPEED)) {
    float prev = text->symbol_anim_speed;
    text->symbol_anim_speed = token_to_float(get_token(&message));
    return text->symbol_anim_speed != prev;
  }

  respond(rsp, "[!] Symbol: Unknown property '%s'\n", entry.text);
  return false;
}

bool text_parse_sub_domain(struct text* text, FILE* rsp, struct token property, char* message) {
  bool needs_refresh = false;
  if (token_equals(property, PROPERTY_COLOR)) {
    struct token token = get_token(&message);
    ANIMATE_BYTES(text_set_color,
                  text,
                  text->color.hex,
                  token_to_int(token));
  }
  else if (token_equals(property, PROPERTY_HIGHLIGHT)) {
    bool highlight = evaluate_boolean_state(get_token(&message),
                                             text->highlight    );
    if (g_bar_manager.animator.duration > 0) {
      if (text->highlight && !highlight) {
        animator_cancel(&g_bar_manager.animator,
                        text,
                        (animator_function*)text_set_color);

        uint32_t target = text->color.hex;
        text_set_color(text, text->highlight_color.hex);

        ANIMATE_BYTES(text_set_color,
                      text,
                      text->color.hex,
                      target          );
      }
      else if (!text->highlight && highlight) {
        animator_cancel(&g_bar_manager.animator,
                        text,
                        (animator_function*)text_set_highlight_color);

        uint32_t target = text->highlight_color.hex;
        text_set_highlight_color(text, text->color.hex);

        ANIMATE_BYTES(text_set_highlight_color,
                      text,
                      text->highlight_color.hex,
                      target                    );
      }
    }

    needs_refresh = text->highlight != highlight;
    text->highlight = highlight;
  } else if (token_equals(property, PROPERTY_FONT))
    needs_refresh = text_set_font(text, string_copy(message), false);
  else if (token_equals(property, PROPERTY_HIGHLIGHT_COLOR)) {
    struct token token = get_token(&message);
    ANIMATE_BYTES(text_set_highlight_color,
                  text,
                  text->highlight_color.hex,
                  token_to_int(token)       );

  } else if (token_equals(property, PROPERTY_PADDING_LEFT)) {
    struct token token = get_token(&message);
    ANIMATE(text_set_padding_left,
            text,
            text->padding_left,
            token_to_int(token)  );

  } else if (token_equals(property, PROPERTY_PADDING_RIGHT)) {
    struct token token = get_token(&message);
    ANIMATE(text_set_padding_right,
            text,
            text->padding_right,
            token_to_int(token)    );

  } else if (token_equals(property, PROPERTY_YOFFSET)) {
    struct token token = get_token(&message);
    ANIMATE(text_set_yoffset,
            text,
            text->y_offset,
            token_to_int(token));

  } else if (token_equals(property, PROPERTY_SCROLL_DURATION)) {
    struct token token = get_token(&message);
    text_set_scroll_duration(text, token_to_int(token));
  } else if (token_equals(property, PROPERTY_WIDTH)) {
    struct token token = get_token(&message);
    if (token_equals(token, ARGUMENT_DYNAMIC)) {
      ANIMATE(text_set_width,
              text,
              text->custom_width,
              text_get_length(text, true));

      struct animation* animation = animation_create();
      animation_setup(animation,
                      text,
                      (bool (*)(void*, int))&text_set_width,
                      text->custom_width,
                      -1,
                      0,
                      INTERP_FUNCTION_LINEAR               );
      animator_add(&g_bar_manager.animator, animation);
    }
    else {
      ANIMATE(text_set_width,
              text,
              text_get_length(text, false),
              token_to_int(token)          );
    }
  } else if (token_equals(property, PROPERTY_DRAWING)) {
    bool prev = text->drawing;
    text->drawing = evaluate_boolean_state(get_token(&message), text->drawing);
    return prev != text->drawing;
  } else if (token_equals(property, PROPERTY_ALIGN)) {
    char prev = text->align;
    text->align = get_token(&message).text[0];
    return prev != text->align;
  } else if (token_equals(property, PROPERTY_STRING)) {
    uint32_t pre_width = text_get_length(text, false);
    bool changed = text_set_string(text,
                                   token_to_string(get_token(&message)),
                                   false                                );

    if (changed
        && g_bar_manager.animator.duration > 0) {
      uint32_t post_width = text_get_length(text, false);
      if (post_width != pre_width) {
        text_set_width(text, pre_width);
        ANIMATE(text_set_width, text, pre_width, post_width);

        struct animation* animation = animation_create();
        animation_setup(animation,
                        text,
                        (bool (*)(void*, int))&text_set_width,
                        text->custom_width,
                        -1,
                        0,
                        INTERP_FUNCTION_LINEAR               );
        animator_add(&g_bar_manager.animator, animation);
      }
    }

    return changed;
  } else if (token_equals(property, PROPERTY_MAX_CHARS)) {
    return text_set_max_chars(text, token_to_int(get_token(&message)));
  }
  else {
    struct key_value_pair key_value_pair = get_key_value_pair(property.text,
                                                              '.'           );
    if (key_value_pair.key && key_value_pair.value) {
      struct token subdom = { key_value_pair.key, strlen(key_value_pair.key) };
      struct token entry = { key_value_pair.value,
                             strlen(key_value_pair.value) };
      if (token_equals(subdom, SUB_DOMAIN_BACKGROUND))
        return background_parse_sub_domain(&text->background,
                                           rsp,
                                           entry,
                                           message           );
      else if (token_equals(subdom, SUB_DOMAIN_SHADOW))
        return shadow_parse_sub_domain(&text->shadow, rsp, entry, message);
      else if (token_equals(subdom, SUB_DOMAIN_FONT))
        return font_parse_sub_domain(&text->font, rsp, entry, message);
      else if (token_equals(subdom, SUB_DOMAIN_COLOR))
        return color_parse_sub_domain(&text->color, rsp, entry, message);
      else if (token_equals(subdom, SUB_DOMAIN_HIGHLIGHT_COLOR))
        return color_parse_sub_domain(&text->highlight_color,
                                      rsp,
                                      entry,
                                      message);
      else if (token_equals(subdom, SUB_DOMAIN_SYMBOL))
        return text_set_symbol_property(text, rsp, entry, message);
      else
        respond(rsp, "[!] Text: Invalid subdomain '%s' \n", subdom.text);
    }
    else {
      respond(rsp, "[!] Text: Invalid property '%s'\n", property.text);
    }
  }

  return needs_refresh;
}
