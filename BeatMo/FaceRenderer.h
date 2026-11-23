#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <math.h>
#include "Config.h"
#include "FaceTypes.h"

// ============================================================================
// High-Speed Clipped U8g2 Face Renderer (128x64 Monochrome SSD1306)
// ============================================================================
namespace FaceRenderer {

// --- Signed-coordinate safe primitives (prevents u8g2_uint_t wrap-around) ---

inline void safePixel(U8G2 &u8g2, int x, int y) {
  if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
    u8g2.drawPixel(static_cast<u8g2_uint_t>(x), static_cast<u8g2_uint_t>(y));
  }
}

inline void safeHLine(U8G2 &u8g2, int x, int y, int w) {
  if (y < 0 || y >= SCREEN_HEIGHT || w <= 0) return;
  int x0 = x;
  int x1 = x + w - 1;
  if (x1 < 0 || x0 >= SCREEN_WIDTH) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= SCREEN_WIDTH) x1 = SCREEN_WIDTH - 1;
  u8g2.drawHLine(static_cast<u8g2_uint_t>(x0),
                 static_cast<u8g2_uint_t>(y),
                 static_cast<u8g2_uint_t>(x1 - x0 + 1));
}

inline void safeVLine(U8G2 &u8g2, int x, int y, int h) {
  if (x < 0 || x >= SCREEN_WIDTH || h <= 0) return;
  int y0 = y;
  int y1 = y + h - 1;
  if (y1 < 0 || y0 >= SCREEN_HEIGHT) return;
  if (y0 < 0) y0 = 0;
  if (y1 >= SCREEN_HEIGHT) y1 = SCREEN_HEIGHT - 1;
  u8g2.drawVLine(static_cast<u8g2_uint_t>(x),
                 static_cast<u8g2_uint_t>(y0),
                 static_cast<u8g2_uint_t>(y1 - y0 + 1));
}

inline void safeBox(U8G2 &u8g2, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  int x0 = x;
  int y0 = y;
  int x1 = x + w - 1;
  int y1 = y + h - 1;
  if (x1 < 0 || x0 >= SCREEN_WIDTH || y1 < 0 || y0 >= SCREEN_HEIGHT) return;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= SCREEN_WIDTH) x1 = SCREEN_WIDTH - 1;
  if (y1 >= SCREEN_HEIGHT) y1 = SCREEN_HEIGHT - 1;
  u8g2.drawBox(static_cast<u8g2_uint_t>(x0),
               static_cast<u8g2_uint_t>(y0),
               static_cast<u8g2_uint_t>(x1 - x0 + 1),
               static_cast<u8g2_uint_t>(y1 - y0 + 1));
}

inline void safeRBox(U8G2 &u8g2, int x, int y, int w, int h, float roundness) {
  if (w < 2) w = 2;
  if (h < 2) h = 2;
  int left = clampi(x, 0, SCREEN_WIDTH - w);
  int top  = clampi(y, 0, SCREEN_HEIGHT - h);

  int minSide = (w < h) ? w : h;
  int r       = static_cast<int>(minSide * clampf(roundness, 0.0f, 0.5f));
  int maxR    = minSide / 2 - 1;
  if (r > maxR) r = maxR;
  if (r < 0) r = 0;

  u8g2.drawRBox(static_cast<u8g2_uint_t>(left),
                static_cast<u8g2_uint_t>(top),
                static_cast<u8g2_uint_t>(w),
                static_cast<u8g2_uint_t>(h),
                static_cast<u8g2_uint_t>(r));
}

inline void safeFilledEllipse(U8G2 &u8g2, int cx, int cy, int rx, int ry) {
  if (rx <= 0 || ry <= 0) return;
  for (int dy = -ry; dy <= ry; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= SCREEN_HEIGHT) continue;
    float ny = static_cast<float>(dy) / static_cast<float>(ry);
    float sq = 1.0f - ny * ny;
    if (sq < 0.0f) continue;
    int dx = static_cast<int>(roundf(static_cast<float>(rx) * sqrtf(sq)));
    safeHLine(u8g2, cx - dx, y, dx * 2 + 1);
  }
}

inline void safeFilledCircle(U8G2 &u8g2, int cx, int cy, int r) {
  safeFilledEllipse(u8g2, cx, cy, r, r);
}

inline void safeLine(U8G2 &u8g2, int x0, int y0, int x1, int y1) {
  // Bresenham clipped per pixel — safe for small UI icons & particles
  int dx  = abs(x1 - x0);
  int sx  = (x0 < x1) ? 1 : -1;
  int dy  = -abs(y1 - y0);
  int sy  = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    safePixel(u8g2, x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// ============================================================================
// Special Eye Drawing Helpers
// ============================================================================

inline void drawHeartEye(U8G2 &u8g2, float cx, float cy, float ew, float eh) {
  float s = clampf((ew < eh ? ew : eh), 8.0f, 36.0f);
  int r   = clampi(static_cast<int>(roundf(s * 0.28f)), 3, 10);
  int lobeY = static_cast<int>(roundf(cy - s * 0.14f));
  int lx    = static_cast<int>(roundf(cx - r * 0.92f));
  int rx    = static_cast<int>(roundf(cx + r * 0.92f));

  u8g2.setDrawColor(1);
  safeFilledCircle(u8g2, lx, lobeY, r);
  safeFilledCircle(u8g2, rx, lobeY, r);

  // Smooth filled lower V-wedge connecting outer edges of both lobes to tip
  int topY = lobeY + 1;
  int botY = static_cast<int>(roundf(cy + s * 0.48f));
  int halfSpan = static_cast<int>(roundf(r * 1.90f));
  int icx = static_cast<int>(roundf(cx));

  if (botY > topY) {
    for (int y = topY; y <= botY; y++) {
      float t = static_cast<float>(y - topY) / static_cast<float>(botY - topY);
      int hw  = static_cast<int>(roundf(halfSpan * (1.0f - t)));
      safeHLine(u8g2, icx - hw, y, hw * 2 + 1);
    }
  }
}

inline void drawDizzyEye(U8G2 &u8g2, float cx, float cy, float ew, float eh,
                         float phase, bool isLeft) {
  float maxR = clampf(((ew < eh) ? ew : eh) * 0.48f, 6.0f, 16.0f);
  float dir  = isLeft ? 1.0f : -1.0f;
  float spin = phase * 7.5f * dir;

  u8g2.setDrawColor(1);

  // Rotating Archimedean spiral (2.25 turns, 2px stroke for bold OLED readability)
  static constexpr int STEPS = 44;
  static constexpr float MAX_THETA = 14.137f;  // 2.25 * 2 * PI
  for (int i = 0; i <= STEPS; i++) {
    float frac  = static_cast<float>(i) / static_cast<float>(STEPS);
    float theta = frac * MAX_THETA * dir + spin;
    float r     = frac * maxR;
    int px      = static_cast<int>(roundf(cx + cosf(theta) * r));
    int py      = static_cast<int>(roundf(cy + sinf(theta) * r));
    safePixel(u8g2, px, py);
    safePixel(u8g2, px + 1, py);
    safePixel(u8g2, px, py + 1);
  }
}

inline void drawEye(U8G2 &u8g2, const AnimatedEye &eye) {
  const float cx = eye.x.val + eye.renderOx;
  const float cy = eye.y.val + eye.renderOy;

  float ew = eye.w.val * eye.renderScaleW;
  float eh = eye.h.val * eye.renderScaleH;

  // Apply organic blink squash-and-stretch
  if (eye.blinkClose > 0.001f) {
    const float openFactor = clampf(1.0f - eye.blinkClose, 0.0f, 1.0f);
    eh = lerpf(2.2f, eh, openFactor);
    ew += eye.blinkClose * 4.0f;
  }

  // Special non-rectangular eye styles
  if (eye.style == EYE_HEART) {
    drawHeartEye(u8g2, cx, cy, ew, eh);
    return;
  }
  if (eye.style == EYE_DIZZY) {
    drawDizzyEye(u8g2, cx, cy, ew, eh, eye.stylePhase, eye.isLeft);
    return;
  }

  const int ww   = clampi(static_cast<int>(roundf(ew)), 3, 44);
  const int hh   = clampi(static_cast<int>(roundf(eh)), 2, 50);
  const int left = clampi(static_cast<int>(roundf(cx - ww * 0.5f)), 0, SCREEN_WIDTH - ww);
  const int top  = clampi(static_cast<int>(roundf(cy - hh * 0.5f)), 0, SCREEN_HEIGHT - hh);

  // Pass 1: White rounded eye base
  u8g2.setDrawColor(1);
  safeRBox(u8g2, left, top, ww, hh, eye.roundness.val);

  // Pass 2: Dynamic black eyelid masks (strictly bounded within this eye!)
  if (hh >= 6 && eye.blinkClose < 0.85f) {
    u8g2.setDrawColor(0);

    // 2a. Top eyelid (supports flat sleepy/suspicious, inward angry, outward sad)
    const float lidL = eye.isLeft ? eye.topOuter.val : eye.topInner.val;
    const float lidR = eye.isLeft ? eye.topInner.val : eye.topOuter.val;
    const float dropL = clampf(lidL, 0.0f, 0.84f) * static_cast<float>(hh);
    const float dropR = clampf(lidR, 0.0f, 0.84f) * static_cast<float>(hh);

    if (dropL > 0.4f || dropR > 0.4f) {
      const float invW = 1.0f / static_cast<float>(ww);
      for (int dx = -1; dx <= ww; dx++) {
        const float t    = clampf(static_cast<float>(dx) * invW, 0.0f, 1.0f);
        const int yCut   = top + static_cast<int>(roundf(lerpf(dropL, dropR, t)));
        const int cutLen = yCut - (top - 1);
        if (cutLen > 0) {
          safeVLine(u8g2, left + dx, top - 1, cutLen);
        }
      }
    }

    // 2b. Bottom curved smiling cheek mask (smoothly carves ^ ^ happy crescents)
    if (eye.bottomLid.val > 0.02f) {
      const float bVal  = clampf(eye.bottomLid.val * (1.0f - eye.blinkClose), 0.0f, 1.0f);
      const float rx    = static_cast<float>(ww) * 0.57f;
      const float ry    = static_cast<float>(hh) * 0.68f;
      const float lift  = bVal * static_cast<float>(hh) * 0.82f;
      const float ellCy = static_cast<float>(top + hh) + ry - lift;
      const float eyeCx = static_cast<float>(left) + static_cast<float>(ww) * 0.5f;

      for (int x = left - 1; x <= left + ww; x++) {
        const float nx = (static_cast<float>(x) - eyeCx) / rx;
        if (fabsf(nx) <= 1.0f) {
          const float dy = ry * sqrtf(1.0f - nx * nx);
          const int yTop = static_cast<int>(roundf(ellCy - dy));
          const int len  = (top + hh + 2) - yTop;
          if (len > 0) {
            safeVLine(u8g2, x, yTop, len);
          }
        }
      }
    }

    // 2c. Scared tiny trembling dark pupil
    if (eye.style == EYE_SCARED_PUPIL && hh >= 14) {
      const int jx = static_cast<int>(roundf(sinf(eye.stylePhase * 41.0f) * 1.1f));
      const int jy = static_cast<int>(roundf(cosf(eye.stylePhase * 37.0f) * 1.1f));
      const int pcx = left + ww / 2 + jx;
      const int pcy = top + hh / 2 + jy;
      safeFilledCircle(u8g2, pcx, pcy, 3);
      u8g2.setDrawColor(1);
      safePixel(u8g2, pcx - 1, pcy - 1);  // tiny panicked glint
    }
  }

  // Always restore white drawing color
  u8g2.setDrawColor(1);
}

// ============================================================================
// Parametric Mouth Drawing
// ============================================================================

inline void drawMouth(U8G2 &u8g2, const AnimatedMouth &mouth) {
  const float cx = mouth.x.val + mouth.renderOx;
  const float cy = mouth.y.val + mouth.renderOy;
  const float mw = clampf(mouth.w.val, 4.0f, 54.0f);
  const float mh = clampf(mouth.h.val, 2.0f, 16.0f);
  const float curve   = clampf(mouth.curve.val, -1.0f, 1.0f);
  const float openAmt = clampf(mouth.open.val + mouth.renderOpenBoost, 0.0f, 1.0f);

  const int icx   = static_cast<int>(roundf(cx));
  const int icy   = static_cast<int>(roundf(cy));
  const int halfW = clampi(static_cast<int>(roundf(mw * 0.5f)), 2, 27);

  u8g2.setDrawColor(1);

  if (mouth.style == MOUTH_OPEN_O) {
    const int rx = clampi(static_cast<int>(roundf(mw * 0.5f)), 3, 12);
    const int ry = clampi(static_cast<int>(roundf(mh * 0.55f)), 3, 9);
    safeFilledEllipse(u8g2, icx, icy, rx, ry);
    if (rx >= 4 && ry >= 4) {
      u8g2.setDrawColor(0);
      safeFilledEllipse(u8g2, icx, icy, rx - 2, ry - 2);
      u8g2.setDrawColor(1);
    }
    return;
  }

  if (mouth.style == MOUTH_CAT) {
    // Cute ":3" / "w" double arch
    for (int dx = -halfW; dx <= halfW; dx++) {
      const float t  = static_cast<float>(dx) / static_cast<float>(halfW);
      const float s  = (t < 0.0f) ? (2.0f * t + 1.0f) : (2.0f * t - 1.0f);
      const float dy = (1.0f - s * s) * 3.2f - 1.4f;
      const int py   = static_cast<int>(roundf(cy + dy));
      safeVLine(u8g2, icx + dx, py, 2);
    }
    return;
  }

  if (mouth.style == MOUTH_WAVY) {
    // Animated sinusoidal nervous / dizzy mouth
    const float amp = clampf(mh * 0.34f, 1.5f, 3.8f);
    for (int dx = -halfW; dx <= halfW; dx++) {
      const float t  = static_cast<float>(dx) / static_cast<float>(halfW);
      const float dy = sinf(t * 7.8f + mouth.wavePhase) * amp;
      const int py   = static_cast<int>(roundf(cy + dy));
      safeVLine(u8g2, icx + dx, py, 2);
    }
    return;
  }

  // Default: MOUTH_CURVE (smoothly morphs frown <-> flat <-> smile <-> open grin)
  const float amp = curve * mh * 0.65f;

  if (openAmt > 0.14f) {
    // Open happy/laughing "D" mouth (sized cleanly to avoid clipping y=63)
    const float depth = openAmt * mh * 0.85f;
    for (int dx = -halfW; dx <= halfW; dx++) {
      const float t = static_cast<float>(dx) / static_cast<float>(halfW);
      const float u = 1.0f - t * t;
      const int yTop = static_cast<int>(roundf(cy - depth * 0.42f + u * amp * 0.15f));
      const int yBot = static_cast<int>(roundf(yTop + 1.5f + u * (depth + fabsf(amp) * 0.25f)));
      if (yBot >= yTop) {
        safeVLine(u8g2, icx + dx, yTop, yBot - yTop + 1);
      }
    }
  } else {
    // Crisp 2-3px curved lip line, tapered gently at the corners
    for (int dx = -halfW; dx <= halfW; dx++) {
      const float t   = static_cast<float>(dx) / static_cast<float>(halfW);
      const float u   = 1.0f - t * t;
      const float yf  = cy + (u - 0.5f) * amp * 2.0f;
      const int py    = static_cast<int>(roundf(yf));
      const int thick = (fabsf(t) > 0.82f) ? 2 : 3;
      safeVLine(u8g2, icx + dx, py - thick / 2, thick);
    }
  }
}

// ============================================================================
// Particle & Overlay Effects (Zzz, Hearts, Sparkles, Tears, Blush, Vein, ?, Sweat)
// ============================================================================

inline void drawMiniZ(U8G2 &u8g2, int x, int y, int sz) {
  safeHLine(u8g2, x, y, sz);
  safeLine(u8g2, x + sz - 1, y, x, y + sz - 1);
  safeHLine(u8g2, x, y + sz - 1, sz);
}

inline void drawMiniHeart(U8G2 &u8g2, int cx, int cy, bool large) {
  if (!large) {
    // 5x5 crisp heart
    safePixel(u8g2, cx - 1, cy - 1);
    safePixel(u8g2, cx + 1, cy - 1);
    safeHLine(u8g2, cx - 2, cy, 5);
    safeHLine(u8g2, cx - 1, cy + 1, 3);
    safePixel(u8g2, cx, cy + 2);
  } else {
    // 7x6 crisp heart
    safeHLine(u8g2, cx - 2, cy - 2, 2);
    safeHLine(u8g2, cx + 1, cy - 2, 2);
    safeHLine(u8g2, cx - 3, cy - 1, 7);
    safeHLine(u8g2, cx - 3, cy, 7);
    safeHLine(u8g2, cx - 2, cy + 1, 5);
    safeHLine(u8g2, cx - 1, cy + 2, 3);
    safePixel(u8g2, cx, cy + 3);
  }
}

inline void drawSparkle(U8G2 &u8g2, int cx, int cy, int arm) {
  if (arm <= 0) return;
  safeHLine(u8g2, cx - arm, cy, arm * 2 + 1);
  safeVLine(u8g2, cx, cy - arm, arm * 2 + 1);
}

inline void drawTeardrop(U8G2 &u8g2, int cx, int cy) {
  // Cute 5x7 teardrop
  safePixel(u8g2, cx, cy - 3);
  safeHLine(u8g2, cx - 1, cy - 2, 3);
  safeHLine(u8g2, cx - 2, cy - 1, 5);
  safeHLine(u8g2, cx - 2, cy, 5);
  safeHLine(u8g2, cx - 1, cy + 1, 3);
}

inline void drawBlush(U8G2 &u8g2, float pulse) {
  const int dy = static_cast<int>(roundf(sinf(pulse * 2.5f) * 0.6f));
  // Left cheek diagonal ticks (///)
  for (int i = 0; i < 3; i++) {
    int bx = 8 + i * 4;
    int by = 43 + dy;
    safeLine(u8g2, bx, by + 4, bx + 3, by);
  }
  // Right cheek diagonal ticks (///)
  for (int i = 0; i < 3; i++) {
    int bx = 109 + i * 4;
    int by = 43 + dy;
    safeLine(u8g2, bx, by + 4, bx + 3, by);
  }
}

inline void drawAngerVein(U8G2 &u8g2, float t) {
  // Pulsing anime forehead anger symbol at top-right corner
  const int cx = 117;
  const int cy = 10;
  const int g  = (sinf(t * 10.0f) > 0.0f) ? 2 : 1;  // center gap pulses 1..2
  const int a  = 3;

  // Top-left corner
  safeHLine(u8g2, cx - g - a, cy - g, a);
  safeVLine(u8g2, cx - g, cy - g - a, a + 1);
  // Top-right corner
  safeHLine(u8g2, cx + g, cy - g, a);
  safeVLine(u8g2, cx + g, cy - g - a, a + 1);
  // Bottom-left corner
  safeHLine(u8g2, cx - g - a, cy + g, a);
  safeVLine(u8g2, cx - g, cy + g, a + 1);
  // Bottom-right corner
  safeHLine(u8g2, cx + g, cy + g, a);
  safeVLine(u8g2, cx + g, cy + g, a + 1);
}

inline void drawQuestionMark(U8G2 &u8g2, float t) {
  const int cx = 117;
  const int cy = 9 + static_cast<int>(roundf(sinf(t * 4.5f) * 2.2f));
  // Crisp custom "?" glyph (7x11px)
  safeHLine(u8g2, cx - 2, cy - 4, 5);
  safePixel(u8g2, cx - 3, cy - 3);
  safePixel(u8g2, cx + 3, cy - 3);
  safePixel(u8g2, cx + 3, cy - 2);
  safePixel(u8g2, cx + 2, cy - 1);
  safeHLine(u8g2, cx, cy, 2);
  safeVLine(u8g2, cx, cy + 1, 2);
  safeBox(u8g2, cx, cy + 5, 2, 2);
}

inline void drawEffects(U8G2 &u8g2, const FaceFX &fx) {
  u8g2.setDrawColor(1);

  if (fx.showBlush) {
    drawBlush(u8g2, fx.fxTime);
  }

  if (fx.showAngerVein) {
    drawAngerVein(u8g2, fx.fxTime);
  }

  if (fx.showQuestion) {
    drawQuestionMark(u8g2, fx.fxTime);
  }

  if (fx.showSweat) {
    const int sy = 8 + static_cast<int>(roundf(fx.sweatPhase * 22.0f));
    drawTeardrop(u8g2, 118, sy);
  }

  if (fx.showTears) {
    for (int i = 0; i < 2; i++) {
      if (fx.tears[i].active) {
        drawTeardrop(u8g2,
                     static_cast<int>(roundf(fx.tears[i].x)),
                     static_cast<int>(roundf(fx.tears[i].y)));
      }
    }
  }

  if (fx.floaterMode != FLOATER_NONE) {
    for (int i = 0; i < 3; i++) {
      const FloatParticle &p = fx.floaters[i];
      const int px = static_cast<int>(roundf(p.x));
      const int py = static_cast<int>(roundf(p.y));

      if (fx.floaterMode == FLOATER_ZZZ) {
        const int sz = (p.phase < 0.35f) ? 4 : ((p.phase < 0.72f) ? 5 : 6);
        drawMiniZ(u8g2, px, py, sz);
      } else if (fx.floaterMode == FLOATER_HEARTS) {
        drawMiniHeart(u8g2, px, py, p.phase > 0.45f);
      } else if (fx.floaterMode == FLOATER_SPARKLES) {
        // Twinkle in and out smoothly
        const float tri = 1.0f - fabsf(p.phase * 2.0f - 1.0f);
        const int arm   = static_cast<int>(roundf(tri * 3.2f));
        drawSparkle(u8g2, px, py, arm);
      }
    }
  }
}

}  // namespace FaceRenderer
