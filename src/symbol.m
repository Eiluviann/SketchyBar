#include "symbol.h"
#include "event.h"
#include <AppKit/AppKit.h>
#include <QuartzCore/QuartzCore.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static float symbol_get_screen_scale(void) {
  float scale = 1.f;
  @autoreleasepool {
    for (NSScreen* screen in [NSScreen screens]) {
      float s = [screen backingScaleFactor];
      if (s > scale) scale = s;
    }
  }
  return scale;
}

static NSFontWeight weight_from_string(const char* w) {
  if (!w)                          return NSFontWeightRegular;
  if (strcmp(w, "ultralight") == 0) return NSFontWeightUltraLight;
  if (strcmp(w, "thin")       == 0) return NSFontWeightThin;
  if (strcmp(w, "light")      == 0) return NSFontWeightLight;
  if (strcmp(w, "medium")     == 0) return NSFontWeightMedium;
  if (strcmp(w, "semibold")   == 0) return NSFontWeightSemibold;
  if (strcmp(w, "bold")       == 0) return NSFontWeightBold;
  if (strcmp(w, "heavy")      == 0) return NSFontWeightHeavy;
  if (strcmp(w, "black")      == 0) return NSFontWeightBlack;
  return NSFontWeightRegular;
}

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
static NSImageSymbolScale scale_from_string(const char* s) {
  if (!s)                   return NSImageSymbolScaleMedium;
  if (strcmp(s, "small") == 0) return NSImageSymbolScaleSmall;
  if (strcmp(s, "large") == 0) return NSImageSymbolScaleLarge;
  return NSImageSymbolScaleMedium;
}

// Build an NSImageSymbolConfiguration from the provided parameters.
static NSImageSymbolConfiguration* build_config(const char* weight,
                                                 const char* scale,
                                                 float size,
                                                 uint8_t rendering,
                                                 float* pr, float* pg,
                                                 float* pb, float* pa,
                                                 int pcount) {
  NSImageSymbolConfiguration* cfg = nil;

  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
  if (@available(macOS 12.0, *)) {
    NSFontWeight fw = weight_from_string(weight);
    NSImageSymbolScale fs = scale_from_string(scale);
    NSImageSymbolConfiguration* ws =
      [NSImageSymbolConfiguration configurationWithPointSize:size
                                                      weight:fw
                                                       scale:fs];
    NSImageSymbolConfiguration* mode_cfg = nil;
    switch (rendering) {
      case SYMBOL_RENDERING_HIERARCHICAL:
        if (pcount >= 1) {
          NSColor* c = [NSColor colorWithRed:pr[0] green:pg[0]
                                        blue:pb[0] alpha:pa[0]];
          mode_cfg = [NSImageSymbolConfiguration
                        configurationWithHierarchicalColor:c];
        }
        break;
      case SYMBOL_RENDERING_PALETTE:
        if (pcount >= 1) {
          NSMutableArray* colors = [NSMutableArray arrayWithCapacity:pcount];
          for (int i = 0; i < pcount; i++)
            [colors addObject:[NSColor colorWithRed:pr[i] green:pg[i]
                                              blue:pb[i] alpha:pa[i]]];
          mode_cfg = [NSImageSymbolConfiguration
                        configurationWithPaletteColors:colors];
        }
        break;
      case SYMBOL_RENDERING_MULTICOLOR:
        mode_cfg = [NSImageSymbolConfiguration
                      configurationPreferringMulticolor];
        break;
      default:
        break;
    }
    cfg = mode_cfg
          ? [ws configurationByApplyingConfiguration:mode_cfg]
          : ws;
  }
  #endif
  return cfg;
}
#endif

// ---------------------------------------------------------------------------
// symbol_create
// ---------------------------------------------------------------------------

CGImageRef symbol_create(const char* name,
                         const char* weight,
                         const char* scale,
                         float size,
                         uint8_t rendering,
                         float* palette_r, float* palette_g,
                         float* palette_b, float* palette_a,
                         int palette_count,
                         bool* is_template_out) {
  @autoreleasepool {
    #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
    if (@available(macOS 11.0, *)) {
      NSImage* img = [NSImage imageWithSystemSymbolName:@(name)
                                  accessibilityDescription:nil];
      if (!img) return NULL;

      NSImageSymbolConfiguration* cfg =
        build_config(weight, scale, size, rendering,
                     palette_r, palette_g, palette_b, palette_a,
                     palette_count);
      if (cfg) img = [img imageWithSymbolConfiguration:cfg];

      if (is_template_out)
        *is_template_out = (rendering == SYMBOL_RENDERING_MONOCHROME);

      float s = symbol_get_screen_scale();
      NSRect rect = NSMakeRect(0, 0, size * s, size * s);
      CGImageRef result = (CGImageRef)CFRetain(
        [img CGImageForProposedRect:&rect context:nil hints:nil]);
      return result;
    }
    #endif
    if (is_template_out) *is_template_out = true;
    return NULL;
  }
}

// ---------------------------------------------------------------------------
// Native NSSymbolEffect animation context (macOS 14+)
// ---------------------------------------------------------------------------

struct symbol_effect_ctx {
  void* obj; // SBSymbolEffectCtx* (ObjC object, stored as void* for C ABI)
};

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
@interface SBSymbolEffectCtx : NSObject
@property (nonatomic) NSImageView*  imageView;
@property (nonatomic) NSWindow*     hiddenWindow;
@property (nonatomic) CADisplayLink* displayLink;
@property (nonatomic) CGImageRef    currentFrame; // atomic read via accessor
@property (nonatomic) CGSize        frameSize;
- (instancetype)initWithName:(NSString*)name
                        size:(CGFloat)size
                      config:(NSImageSymbolConfiguration*)cfg
                    animType:(uint8_t)animType
                   layerMode:(uint8_t)layerMode
                      repeat:(BOOL)repeat
                       speed:(float)speed API_AVAILABLE(macos(14.0));
- (void)stop;
@end

@implementation SBSymbolEffectCtx {
  CGImageRef _currentFrame;
  OSSpinLock _frameLock;
}

- (instancetype)initWithName:(NSString*)name
                        size:(CGFloat)size
                      config:(NSImageSymbolConfiguration*)cfg
                    animType:(uint8_t)animType
                   layerMode:(uint8_t)layerMode
                      repeat:(BOOL)repeat
                       speed:(float)speed API_AVAILABLE(macos(14.0)) {
  if (!(self = [super init])) return nil;
  _frameLock = OS_SPINLOCK_INIT;
  _currentFrame = NULL;
  _frameSize = CGSizeMake(size, size);

  float screen_scale = symbol_get_screen_scale();
  CGFloat px = size * screen_scale;

  // Hidden off-screen window so Core Animation runs properly
  _hiddenWindow = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(-px * 2, -px * 2, px, px)
                styleMask:NSWindowStyleMaskBorderless
                  backing:NSBackingStoreBuffered
                    defer:NO];
  _hiddenWindow.hasShadow = NO;
  _hiddenWindow.backgroundColor = [NSColor clearColor];
  _hiddenWindow.opaque = NO;
  _hiddenWindow.collectionBehavior =
      NSWindowCollectionBehaviorCanJoinAllSpaces |
      NSWindowCollectionBehaviorStationary |
      NSWindowCollectionBehaviorIgnoresCycle;
  [_hiddenWindow setIgnoresMouseEvents:YES];
  [_hiddenWindow orderOut:nil];

  // Image view
  _imageView = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, px, px)];
  _imageView.wantsLayer = YES;
  _imageView.imageScaling = NSImageScaleProportionallyUpOrDown;

  NSImage* img = [NSImage imageWithSystemSymbolName:name
                              accessibilityDescription:nil];
  if (cfg) img = [img imageWithSymbolConfiguration:cfg];
  _imageView.image = img;

  [_hiddenWindow.contentView addSubview:_imageView];
  [_hiddenWindow makeKeyAndOrderFront:nil];
  [_hiddenWindow orderOut:nil]; // hide immediately after activating layer

  // Build effect
  NSSymbolEffect* effect = nil;
  NSSymbolEffectOptions* opts = [NSSymbolEffectOptions options];
  if (speed != 1.f) opts = [opts optionsWithSpeed:speed];
  if (!repeat) opts = [opts optionsWithNonRepeating];

  switch (animType) {
    case SYMBOL_ANIM_DRAW:
      effect = [NSSymbolDrawOnEffect effect];
      break;
    case SYMBOL_ANIM_WIGGLE:
      effect = [NSSymbolWiggleEffect effect];
      break;
    case SYMBOL_ANIM_BREATHE:
      effect = [NSSymbolBreatheEffect effect];
      break;
    case SYMBOL_ANIM_ROTATE:
      effect = [NSSymbolRotateEffect effect];
      break;
    case SYMBOL_ANIM_APPEAR:
      effect = [NSSymbolAppearEffect effect];
      break;
    case SYMBOL_ANIM_DISAPPEAR:
      effect = [NSSymbolDisappearEffect effect];
      break;
    case SYMBOL_ANIM_REPLACE:
      // NSSymbolReplaceEffect does not exist: "replace" is a content transition
      // (NSSymbolReplaceContentTransition), applied via
      // setSymbolImage:withContentTransition: and requiring a target image, so it
      // cannot be expressed through addSymbolEffect:. Fall back to bounce until a
      // dedicated content-transition path is implemented.
      effect = [NSSymbolBounceEffect effect];
      break;
    default:
      effect = [NSSymbolBounceEffect effect];
      break;
  }

  // Apply layer mode
  if (layerMode == SYMBOL_LAYER_BY_LAYER && [effect respondsToSelector:@selector(effectWithByLayer)])
    effect = [effect performSelector:@selector(effectWithByLayer)];
  else if (layerMode == SYMBOL_LAYER_INDIVIDUALLY && [effect respondsToSelector:@selector(effectWithIndividuallyAnimatedLayers)])
    effect = [effect performSelector:@selector(effectWithIndividuallyAnimatedLayers)];

  [_imageView addSymbolEffect:effect options:opts animated:YES];

  // CADisplayLink for frame capture
  // displayLinkWithTarget:selector: is an NSView *instance* method (macOS 14+),
  // not a class method; sending it to the NSView class throws
  // 'unrecognized selector sent to class' and crashes the bar.
  _displayLink = [_imageView displayLinkWithTarget:self selector:@selector(captureFrame:)];
  [_displayLink addToRunLoop:[NSRunLoop mainRunLoop]
                     forMode:NSRunLoopCommonModes];

  return self;
}

- (void)captureFrame:(CADisplayLink*)link {
  @autoreleasepool {
    CGFloat px = _frameSize.width * symbol_get_screen_scale();
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bmp = CGBitmapContextCreate(NULL, (size_t)px, (size_t)px, 8,
                                             (size_t)px * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!bmp) return;

    NSGraphicsContext* gc = [NSGraphicsContext graphicsContextWithCGContext:bmp
                                                                    flipped:YES];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:gc];
    [_imageView.layer renderInContext:bmp];
    [NSGraphicsContext restoreGraphicsState];

    CGImageRef newFrame = CGBitmapContextCreateImage(bmp);
    CGContextRelease(bmp);

    OSSpinLockLock(&_frameLock);
    CGImageRef old = _currentFrame;
    _currentFrame = newFrame;
    OSSpinLockUnlock(&_frameLock);
    if (old) CGImageRelease(old);

    struct event event = { NULL, ANIMATOR_REFRESH };
    event_post(&event);
  }
}

- (CGImageRef)currentFrame {
  OSSpinLockLock(&_frameLock);
  CGImageRef f = _currentFrame;
  OSSpinLockUnlock(&_frameLock);
  return f;
}

- (void)stop {
  [_displayLink invalidate];
  _displayLink = nil;
  [_hiddenWindow close];
  _hiddenWindow = nil;
  _imageView = nil;
  OSSpinLockLock(&_frameLock);
  if (_currentFrame) { CGImageRelease(_currentFrame); _currentFrame = NULL; }
  OSSpinLockUnlock(&_frameLock);
}

- (void)dealloc {
  [self stop];
  [super dealloc];
}

@end
#endif // macOS 14+

symbol_effect_ctx* symbol_effect_start(const char* name,
                                       const char* weight,
                                       const char* scale,
                                       float size,
                                       uint8_t rendering,
                                       float* palette_r, float* palette_g,
                                       float* palette_b, float* palette_a,
                                       int palette_count,
                                       uint8_t anim_type,
                                       uint8_t layer_mode,
                                       bool repeat,
                                       float speed) {
  @autoreleasepool {
    #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
    if (@available(macOS 14.0, *)) {
      NSImageSymbolConfiguration* cfg =
        build_config(weight, scale, size, rendering,
                     palette_r, palette_g, palette_b, palette_a,
                     palette_count);

      SBSymbolEffectCtx* obj =
        [[SBSymbolEffectCtx alloc] initWithName:@(name)
                                           size:size
                                         config:cfg
                                       animType:anim_type
                                      layerMode:layer_mode
                                         repeat:(BOOL)repeat
                                          speed:speed];
      if (!obj) return NULL;

      symbol_effect_ctx* ctx = malloc(sizeof(symbol_effect_ctx));
      ctx->obj = (void*)obj;
      return ctx;
    }
    #endif
    return NULL;
  }
}

void symbol_effect_stop(symbol_effect_ctx* ctx) {
  if (!ctx) return;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
  if (@available(macOS 14.0, *)) {
    @autoreleasepool {
      SBSymbolEffectCtx* obj = (SBSymbolEffectCtx*)ctx->obj;
      [obj stop];
      [obj release];
    }
  }
  #endif
  free(ctx);
}

CGImageRef symbol_effect_current_frame(symbol_effect_ctx* ctx) {
  if (!ctx || !ctx->obj) return NULL;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
  if (@available(macOS 14.0, *)) {
    SBSymbolEffectCtx* obj = (SBSymbolEffectCtx*)ctx->obj;
    return [obj currentFrame];
  }
  #endif
  return NULL;
}
