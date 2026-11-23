/*
 * ============================================================
 *  LyriMo — ESP32 DevKit + OLED SSD1306 128x64 (U8g2) Hybrid Word-Sync Lyric Display
 * ============================================================
 *
 *  WIRING:
 *    OLED VCC -> 3V3
 *    OLED GND -> GND
 *    OLED SDA -> GPIO21
 *    OLED SCL -> GPIO22
 *
 *  BOARD : ESP32 Dev Module   |   PORT : COM6   |   BAUD : 115200
 *  Library : U8g2 by oliver (v2.37.1)
 *
 *  FITUR:
 *    1) MODE_LIVE_SERIAL : Menerima judul, lirik kata/silabel (paket W|),
 *       dan posisi lagu real-time dari pc_bridge/lyric_bridge.py (BitChord).
 *    2) MODE_OFFLINE_DEMO: Otomatis memutar lirik demo dari demo_lyrics.h
 *       dengan animasi kata-per-kata proporsional saat USB tidak terhubung.
 *    3) Animasi OLED 25 FPS: Huruf belum dinyanyikan tampil redup (dither),
 *       menyala terang piksel-demi-piksel & huruf-demi-huruf saat dinyanyikan,
 *       disertai garis progres tipis + slide-up halus saat pergantian baris.
 *
 *  Kalau OLED-mu alamat I2C-nya 0x3D, aktifkan u8g2.setI2CAddress(0x3D * 2);
 * ============================================================
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "demo_lyrics.h"

// Full buffer + hardware I2C (clock SCL=22, data SDA=21)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0, U8X8_PIN_NONE, /* clock=*/22, /* data=*/21);
// Jika OLED ternyata chip SH1106 (1.3"), ganti constructor di atas dengan:
// U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(
//     U8G2_R0, U8X8_PIN_NONE, /* clock=*/22, /* data=*/21);

// ---------- Konstanta Tata Letak 128x64 (12 + 42 + 10 = 64 px) ----------
static const uint32_t SERIAL_BAUD = 115200UL;
static const uint32_t SERIAL_TIMEOUT_MS = 6000UL;
static const uint32_t FRAME_INTERVAL_MS = 40UL;
static const uint32_t SLIDE_ANIM_MS = 160UL;

static const uint16_t HEADER_CLIP_X0 = 10;
static const uint16_t HEADER_CLIP_X1 = 128;
static const int16_t HEADER_VIEW_WIDTH = 118;
static const uint32_t HEADER_PAUSE_MS = 1500UL;
static const uint32_t HEADER_SPEED_PX_S = 24UL;
static const int16_t HEADER_GAP_PX = 28;

static const uint16_t LYRIC_MAX_WIDTH = 124;
static const size_t MAX_LYRIC_LINES = 4;
static const size_t LYRIC_LINE_BUF = 48;
static const size_t MAX_LYRIC_SEGS = 48;

// ---------- Segmen Kata / Silabel ----------
struct LyricSegment {
  uint32_t relStartMs;
  uint32_t relEndMs;
  uint16_t charEnd;
};

// ---------- State Hybrid Mode ----------
enum DisplayMode { MODE_OFFLINE_DEMO, MODE_LIVE_SERIAL };
static DisplayMode displayMode = MODE_OFFLINE_DEMO;

static char serialLineBuf[512];
static size_t serialLineLen = 0;
static bool serialOverflow = false;

static char liveHeader[128] = "BitChord Bridge";
static char liveLyric[160] = "";
static uint32_t liveLineStartMs = 0;
static uint32_t liveLineEndMs = 0;
static LyricSegment liveSegs[MAX_LYRIC_SEGS];
static uint8_t liveSegCount = 0;

static uint32_t livePosMs = 0;
static uint32_t liveDurMs = 0;
static bool livePlaying = false;
static uint32_t liveSyncMillis = 0;
static uint32_t lastSerialRxMillis = 0;

static uint32_t demoStartMillis = 0;
static int16_t lastDemoIndex = -1;
static uint32_t demoLineStartMs = 0;
static LyricSegment demoSegs[MAX_LYRIC_SEGS];
static uint8_t demoSegCount = 0;

static uint32_t lastFrameMillis = 0;

// ---------- Cache Marquee Header ----------
static char cachedHeader[128] = "";
static int16_t cachedHeaderWidth = 0;
static uint32_t headerChangeMillis = 0;

// ---------- Cache Word-Wrap Lirik + Animasi Slide ----------
struct WrappedLyricCache {
  char source[160];
  char lines[MAX_LYRIC_LINES][LYRIC_LINE_BUF];
  uint16_t lineCharStart[MAX_LYRIC_LINES];
  uint16_t lineCharEnd[MAX_LYRIC_LINES];
  uint8_t lineCount;
  const uint8_t* font;
  uint8_t lineHeight;
};

static WrappedLyricCache lyricCache = {"", {{""}}, {0}, {0}, 0, u8g2_font_helvB10_tr, 13};
static uint32_t lyricAnimStartMs = 0;

static void safeStrCopy(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

static void formatTimeMMSS(uint32_t ms, char* out, size_t outSize) {
  uint32_t totalSec = ms / 1000UL;
  if (totalSec > 5999UL) {
    totalSec = 5999UL;
  }
  uint32_t minutes = totalSec / 60UL;
  uint32_t seconds = totalSec % 60UL;
  snprintf(out, outSize, "%02lu:%02lu",
           (unsigned long)minutes, (unsigned long)seconds);
}

static void buildDemoSegments(const char* text, uint32_t singDurMs,
                              LyricSegment* outSegs, uint8_t& outCount) {
  outCount = 0;
  if (text == nullptr || text[0] == '\0') return;

  size_t totalChars = strlen(text);
  size_t tokenEnds[MAX_LYRIC_SEGS];
  uint16_t tokenWeights[MAX_LYRIC_SEGS];
  uint8_t tokenCount = 0;
  uint32_t totalWeight = 0;

  size_t idx = 0;
  while (text[idx] != '\0' && tokenCount < MAX_LYRIC_SEGS) {
    size_t wordStart = idx;
    while (text[idx] != '\0' && text[idx] != ' ') idx++;
    uint16_t w = (uint16_t)(idx - wordStart);
    if (w == 0) w = 1;
    while (text[idx] == ' ') idx++;

    tokenEnds[tokenCount] = idx;
    tokenWeights[tokenCount] = w;
    totalWeight += w;
    tokenCount++;
  }

  if (tokenCount == 0 || totalWeight == 0) return;

  uint32_t cursorMs = 0;
  for (uint8_t i = 0; i < tokenCount; i++) {
    uint32_t endMs = singDurMs;
    if (i + 1 < tokenCount) {
      uint32_t span = (singDurMs * tokenWeights[i]) / totalWeight;
      if (span < 80UL) span = 80UL;
      endMs = cursorMs + span;
      if (endMs > singDurMs) endMs = singDurMs;
    }
    outSegs[outCount].relStartMs = cursorMs;
    outSegs[outCount].relEndMs = (endMs > cursorMs + 30UL) ? endMs : (cursorMs + 30UL);
    outSegs[outCount].charEnd = (uint16_t)tokenEnds[i];
    outCount++;
    cursorMs = endMs;
  }
  if (outCount > 0) {
    outSegs[outCount - 1].charEnd = (uint16_t)totalChars;
  }
}

static void parseWordPacket(const char* payload) {
  // Format: <start_ms>|<end_ms>|<text>|<s0>:<e0>:<c0>,<s1>:<e1>:<c1>,...
  const char* sep1 = strchr(payload, '|');
  if (sep1 == nullptr) return;
  const char* p2 = sep1 + 1;
  const char* sep2 = strchr(p2, '|');
  if (sep2 == nullptr) return;
  const char* p3 = sep2 + 1;
  const char* sep3 = strchr(p3, '|');

  uint32_t startMs = strtoul(payload, nullptr, 10);
  uint32_t endMs = strtoul(p2, nullptr, 10);

  size_t textLen = (sep3 != nullptr) ? (size_t)(sep3 - p3) : strlen(p3);
  if (textLen >= sizeof(liveLyric)) textLen = sizeof(liveLyric) - 1;
  memcpy(liveLyric, p3, textLen);
  liveLyric[textLen] = '\0';

  liveLineStartMs = startMs;
  liveLineEndMs = endMs;
  liveSegCount = 0;

  if (sep3 == nullptr || sep3[1] == '\0') {
    return;
  }

  const char* cur = sep3 + 1;
  while (*cur != '\0' && liveSegCount < MAX_LYRIC_SEGS) {
    char* endPtr = nullptr;
    uint32_t relS = strtoul(cur, &endPtr, 10);
    if (endPtr == cur || *endPtr != ':') break;
    cur = endPtr + 1;

    uint32_t relE = strtoul(cur, &endPtr, 10);
    if (endPtr == cur || *endPtr != ':') break;
    cur = endPtr + 1;

    uint32_t charEnd = strtoul(cur, &endPtr, 10);
    if (endPtr == cur) break;

    if (charEnd > textLen) charEnd = (uint32_t)textLen;
    if (relE < relS) relE = relS;

    liveSegs[liveSegCount].relStartMs = relS;
    liveSegs[liveSegCount].relEndMs = relE;
    liveSegs[liveSegCount].charEnd = (uint16_t)charEnd;
    liveSegCount++;

    if (*endPtr == ',') {
      cur = endPtr + 1;
    } else {
      break;
    }
  }
}

static void processSerialLine(const char* line, uint32_t nowMs) {
  if (line == nullptr || line[0] == '\0') return;

  if (strcmp(line, "B|OFFLINE") == 0) {
    displayMode = MODE_OFFLINE_DEMO;
    demoStartMillis = nowMs;
    lastDemoIndex = -1;
    return;
  }

  if (line[1] != '|') return;

  char cmd = line[0];
  const char* payload = line + 2;

  if (cmd == 'T') {
    safeStrCopy(liveHeader, sizeof(liveHeader), payload);
    displayMode = MODE_LIVE_SERIAL;
    lastSerialRxMillis = nowMs;
  } else if (cmd == 'L') {
    safeStrCopy(liveLyric, sizeof(liveLyric), payload);
    liveSegCount = 0;
    liveLineStartMs = 0;
    liveLineEndMs = 0;
    displayMode = MODE_LIVE_SERIAL;
    lastSerialRxMillis = nowMs;
  } else if (cmd == 'W') {
    parseWordPacket(payload);
    displayMode = MODE_LIVE_SERIAL;
    lastSerialRxMillis = nowMs;
  } else if (cmd == 'P') {
    const char* sep1 = strchr(payload, '|');
    if (sep1 == nullptr) return;
    const char* p2 = sep1 + 1;
    const char* sep2 = strchr(p2, '|');
    if (sep2 == nullptr) return;
    const char* p3 = sep2 + 1;

    livePosMs = strtoul(payload, nullptr, 10);
    liveDurMs = strtoul(p2, nullptr, 10);
    livePlaying = (atoi(p3) != 0);
    liveSyncMillis = nowMs;
    displayMode = MODE_LIVE_SERIAL;
    lastSerialRxMillis = nowMs;
  } else if (cmd == 'S') {
    safeStrCopy(liveHeader, sizeof(liveHeader), "BitChord Bridge");
    safeStrCopy(liveLyric, sizeof(liveLyric), payload);
    liveSegCount = 0;
    liveLineStartMs = 0;
    liveLineEndMs = 0;
    livePosMs = 0;
    liveDurMs = 0;
    livePlaying = false;
    liveSyncMillis = nowMs;
    displayMode = MODE_LIVE_SERIAL;
    lastSerialRxMillis = nowMs;
  }
}

static void pollSerialInput(uint32_t nowMs) {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;

    if (c == '\r') continue;
    if (c == '\n') {
      if (!serialOverflow && serialLineLen > 0) {
        serialLineBuf[serialLineLen] = '\0';
        processSerialLine(serialLineBuf, nowMs);
      }
      serialLineLen = 0;
      serialOverflow = false;
      continue;
    }

    if (serialOverflow) continue;

    if (serialLineLen < sizeof(serialLineBuf) - 1) {
      serialLineBuf[serialLineLen++] = (char)c;
    } else {
      serialOverflow = true;
    }
  }

  if (displayMode == MODE_LIVE_SERIAL &&
      (nowMs - lastSerialRxMillis) > SERIAL_TIMEOUT_MS) {
    displayMode = MODE_OFFLINE_DEMO;
    demoStartMillis = nowMs;
    lastDemoIndex = -1;
  }
}

static int16_t selectDemoIndex(uint32_t posMs) {
  int16_t best = 0;
  for (size_t i = 0; i < DEMO_LYRIC_COUNT; i++) {
    if (DEMO_LYRICS[i].timeMs <= posMs) {
      best = (int16_t)i;
    } else {
      break;
    }
  }
  return best;
}

static bool tryWrapWithFont(const char* text, const uint8_t* font,
                            uint8_t maxLines, uint16_t maxWidth,
                            WrappedLyricCache& out, bool truncateOverflow) {
  u8g2.setFont(font);
  out.font = font;
  out.lineCount = 0;

  size_t idx = 0;
  while (text[idx] == ' ') idx++;

  while (text[idx] != '\0' && out.lineCount < maxLines) {
    char line[LYRIC_LINE_BUF] = "";
    size_t lineLen = 0;
    size_t lineStartIdx = idx;
    size_t lineEndIdx = idx;

    while (text[idx] != '\0') {
      if (lineLen == 0) {
        while (text[idx] == ' ') idx++;
        if (text[idx] == '\0') break;
        lineStartIdx = idx;
      }

      size_t wordStart = idx;
      while (text[idx] != '\0' && text[idx] != ' ') idx++;
      size_t wordLen = idx - wordStart;

      char candidate[LYRIC_LINE_BUF];
      size_t sepLen = (lineLen > 0) ? 1 : 0;
      if (lineLen + sepLen + wordLen < LYRIC_LINE_BUF) {
        if (lineLen > 0) {
          snprintf(candidate, sizeof(candidate), "%s %.*s", line, (int)wordLen,
                   text + wordStart);
        } else {
          snprintf(candidate, sizeof(candidate), "%.*s", (int)wordLen,
                   text + wordStart);
        }

        if (u8g2.getStrWidth(candidate) <= maxWidth) {
          safeStrCopy(line, sizeof(line), candidate);
          lineLen = strlen(line);
          lineEndIdx = idx;
          while (text[idx] == ' ') idx++;
          continue;
        }
      }

      if (lineLen > 0) {
        idx = wordStart;
        break;
      }

      // Satu kata tunggal lebih lebar dari maxWidth: pecah per karakter.
      size_t take = 0;
      while (take < wordLen && (take + 1) < LYRIC_LINE_BUF) {
        candidate[take] = text[wordStart + take];
        candidate[take + 1] = '\0';
        if (take > 0 && u8g2.getStrWidth(candidate) > maxWidth) {
          break;
        }
        take++;
      }
      if (take == 0) take = 1;
      memcpy(line, text + wordStart, take);
      line[take] = '\0';
      lineEndIdx = wordStart + take;
      idx = wordStart + take;
      while (text[idx] == ' ') idx++;
      break;
    }

    if (line[0] != '\0') {
      safeStrCopy(out.lines[out.lineCount], LYRIC_LINE_BUF, line);
      out.lineCharStart[out.lineCount] = (uint16_t)lineStartIdx;
      out.lineCharEnd[out.lineCount] = (uint16_t)lineEndIdx;
      out.lineCount++;
    }
  }

  while (text[idx] == ' ') idx++;
  bool allFit = (text[idx] == '\0');

  if (!allFit && truncateOverflow && out.lineCount > 0) {
    char* last = out.lines[out.lineCount - 1];
    size_t len = strlen(last);
    while (len > 0) {
      if (len + 3 < LYRIC_LINE_BUF) {
        char ell[LYRIC_LINE_BUF];
        snprintf(ell, sizeof(ell), "%s...", last);
        if (u8g2.getStrWidth(ell) <= maxWidth) {
          safeStrCopy(last, LYRIC_LINE_BUF, ell);
          return false;
        }
      }
      last[--len] = '\0';
    }
    safeStrCopy(last, LYRIC_LINE_BUF, "...");
  }

  return allFit;
}

static void updateLyricCache(const char* text, uint32_t nowMs) {
  if (strcmp(lyricCache.source, text) == 0) return;

  safeStrCopy(lyricCache.source, sizeof(lyricCache.source), text);
  lyricAnimStartMs = nowMs;

  if (text[0] == '\0') {
    lyricCache.lineCount = 0;
    return;
  }

  if (tryWrapWithFont(text, u8g2_font_helvB10_tr, 3, LYRIC_MAX_WIDTH,
                      lyricCache, false)) {
    lyricCache.lineHeight = 13;
    return;
  }

  tryWrapWithFont(text, u8g2_font_helvB08_tr, 4, LYRIC_MAX_WIDTH,
                  lyricCache, true);
  lyricCache.lineHeight = 10;
}

static float computeSungCharProgress(uint32_t displayPosMs, uint32_t lineStartMs,
                                     const LyricSegment* segs, uint8_t segCount) {
  if (segCount == 0 || segs == nullptr) {
    return -1.0f;
  }
  uint32_t relMs = (displayPosMs >= lineStartMs) ? (displayPosMs - lineStartMs) : 0UL;
  float sungChar = 0.0f;

  for (uint8_t k = 0; k < segCount; k++) {
    uint16_t prevChar = (k == 0) ? 0 : segs[k - 1].charEnd;
    if (relMs >= segs[k].relEndMs) {
      sungChar = (float)segs[k].charEnd;
    } else if (relMs >= segs[k].relStartMs) {
      uint32_t span = segs[k].relEndMs - segs[k].relStartMs;
      float frac = (span > 0UL) ? ((float)(relMs - segs[k].relStartMs) / (float)span) : 1.0f;
      sungChar = (float)prevChar + (float)(segs[k].charEnd - prevChar) * frac;
      break;
    } else {
      break;
    }
  }
  return sungChar;
}

static int16_t computeLineWipePx(uint8_t rowIdx, int16_t fullLineW, float sungChar) {
  uint16_t cStart = lyricCache.lineCharStart[rowIdx];
  uint16_t cEnd = lyricCache.lineCharEnd[rowIdx];
  if (sungChar <= (float)cStart) return 0;
  if (sungChar >= (float)cEnd) return fullLineW;

  const char* lineStr = lyricCache.lines[rowIdx];
  size_t lineLen = strlen(lineStr);
  float localPos = sungChar - (float)cStart;
  size_t whole = (size_t)localPos;
  if (whole > lineLen) whole = lineLen;
  float subFrac = localPos - (float)whole;

  char prefix[LYRIC_LINE_BUF];
  memcpy(prefix, lineStr, whole);
  prefix[whole] = '\0';
  int16_t w0 = (int16_t)u8g2.getStrWidth(prefix);

  if (whole < lineLen) {
    prefix[whole] = lineStr[whole];
    prefix[whole + 1] = '\0';
    int16_t w1 = (int16_t)u8g2.getStrWidth(prefix);
    int16_t px = w0 + (int16_t)roundf((float)(w1 - w0) * subFrac);
    if (px > fullLineW) px = fullLineW;
    return (px < 0) ? 0 : px;
  }
  return (w0 > fullLineW) ? fullLineW : w0;
}

static void dimRectInBuffer(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  if (x0 < 0) x0 = 0;
  if (x1 > 128) x1 = 128;
  if (y0 < 12) y0 = 12;
  if (y1 > 54) y1 = 54;
  if (x0 >= x1 || y0 >= y1) return;

  uint8_t* buf = u8g2.getBufferPtr();
  if (buf == nullptr) return;

  for (int16_t y = y0; y < y1; y++) {
    uint16_t rowOffset = (uint16_t)(y >> 3) << 7;
    uint8_t bitClearMask = (uint8_t)~(1U << (y & 7));
    for (int16_t x = x0; x < x1; x++) {
      if (((x + y) & 1) != 0) {
        buf[rowOffset + (uint16_t)x] &= bitClearMask;
      }
    }
  }
}

static void drawClippedHeaderRun(int16_t startX, const char* text) {
  int16_t cx = startX;
  char glyphBuf[2] = {'\0', '\0'};
  for (size_t i = 0; text[i] != '\0'; i++) {
    glyphBuf[0] = text[i];
    int16_t gw = (int16_t)u8g2.getStrWidth(glyphBuf);
    if (gw <= 0) gw = 6;

    if (cx + gw <= (int16_t)HEADER_CLIP_X0) {
      cx += gw;
      continue;
    }
    if (cx >= (int16_t)HEADER_CLIP_X1) {
      break;
    }
    if (cx >= 0) {
      u8g2.drawStr((u8g2_uint_t)cx, 1, glyphBuf);
    }
    cx += gw;
  }
}

static void drawHeader(const char* headerText, bool isPlaying, uint32_t nowMs) {
  u8g2.setDrawColor(1);
  u8g2.setFontMode(1);

  if (isPlaying) {
    u8g2.drawTriangle(1, 2, 1, 8, 6, 5);
  } else {
    u8g2.drawBox(1, 2, 2, 7);
    u8g2.drawBox(5, 2, 2, 7);
  }

  u8g2.setFont(u8g2_font_6x10_tf);
  if (strcmp(cachedHeader, headerText) != 0) {
    safeStrCopy(cachedHeader, sizeof(cachedHeader), headerText);
    cachedHeaderWidth = (int16_t)u8g2.getStrWidth(cachedHeader);
    headerChangeMillis = nowMs;
  }

  u8g2.setClipWindow(HEADER_CLIP_X0, 0, HEADER_CLIP_X1, 11);

  if (cachedHeaderWidth <= HEADER_VIEW_WIDTH) {
    u8g2.drawStr(HEADER_CLIP_X0, 1, cachedHeader);
  } else {
    uint32_t elapsed = nowMs - headerChangeMillis;
    int16_t offset = 0;
    int16_t cycle = cachedHeaderWidth + HEADER_GAP_PX;
    if (elapsed > HEADER_PAUSE_MS) {
      uint32_t scrollMs = elapsed - HEADER_PAUSE_MS;
      offset = (int16_t)(((scrollMs * HEADER_SPEED_PX_S) / 1000UL) % (uint32_t)cycle);
    }
    int16_t x1 = (int16_t)HEADER_CLIP_X0 - offset;
    int16_t x2 = x1 + cycle;
    drawClippedHeaderRun(x1, cachedHeader);
    if (x2 < (int16_t)HEADER_CLIP_X1) {
      drawClippedHeaderRun(x2, cachedHeader);
    }
  }

  u8g2.setMaxClipWindow();
  u8g2.drawHLine(0, 11, 128);
}

static void drawInstrumentalGraphic(bool isPlaying, uint32_t nowMs) {
  int16_t yOff = isPlaying ? (((nowMs / 450UL) % 2UL == 0UL) ? -1 : 1) : 0;

  u8g2.drawHLine(40, 33 + yOff, 7);
  u8g2.drawHLine(81, 33 + yOff, 7);

  u8g2.drawDisc(56, 39 + yOff, 3, U8G2_DRAW_ALL);
  u8g2.drawDisc(69, 37 + yOff, 3, U8G2_DRAW_ALL);
  u8g2.drawVLine(59, 25 + yOff, 14);
  u8g2.drawVLine(72, 23 + yOff, 14);
  u8g2.drawLine(59, 25 + yOff, 72, 23 + yOff);
  u8g2.drawLine(59, 26 + yOff, 72, 24 + yOff);
}

static void drawLyricArea(const char* lyricText, bool isPlaying, uint32_t nowMs,
                          uint32_t displayPosMs, uint32_t lineStartMs,
                          const LyricSegment* segs, uint8_t segCount) {
  updateLyricCache(lyricText, nowMs);

  u8g2.setDrawColor(1);
  u8g2.setFontMode(1);
  u8g2.setClipWindow(0, 12, 128, 54);

  if (lyricCache.lineCount == 0) {
    drawInstrumentalGraphic(isPlaying, nowMs);
  } else {
    int16_t slideY = 0;
    uint32_t animElapsed = nowMs - lyricAnimStartMs;
    if (animElapsed < SLIDE_ANIM_MS) {
      float rem = 1.0f - ((float)animElapsed / (float)SLIDE_ANIM_MS);
      slideY = (int16_t)roundf(10.0f * rem * rem);
    }

    u8g2.setFont(lyricCache.font);
    int16_t totalH = (int16_t)lyricCache.lineCount * (int16_t)lyricCache.lineHeight;
    int16_t startY = 13 + (40 - totalH) / 2;
    if (startY < 13) startY = 13;

    float sungChar = computeSungCharProgress(displayPosMs, lineStartMs, segs, segCount);

    for (uint8_t i = 0; i < lyricCache.lineCount; i++) {
      int16_t w = (int16_t)u8g2.getStrWidth(lyricCache.lines[i]);
      int16_t x = (128 - w) / 2;
      if (x < 0) x = 0;
      int16_t y = startY + (int16_t)i * (int16_t)lyricCache.lineHeight + slideY;

      u8g2.drawStr(x, y, lyricCache.lines[i]);

      if (sungChar >= 0.0f) {
        int16_t wipePx = computeLineWipePx(i, w, sungChar);
        if (wipePx < w) {
          dimRectInBuffer(x + wipePx, y, x + w + 1, y + lyricCache.lineHeight);
        }
        bool isActiveRow =
            (sungChar >= (float)lyricCache.lineCharStart[i]) &&
            (sungChar < (float)lyricCache.lineCharEnd[i]);
        if (isActiveRow && wipePx > 0) {
          int16_t uy = y + lyricCache.lineHeight - 1;
          if (uy >= 12 && uy < 54) {
            u8g2.drawHLine(x, uy, wipePx);
          }
        }
      }
    }
  }

  u8g2.setMaxClipWindow();
}

static void drawProgressBar(uint32_t posMs, uint32_t durMs) {
  char leftTime[8];
  char rightTime[8];
  uint32_t clampedPos = (durMs > 0 && posMs > durMs) ? durMs : posMs;

  formatTimeMMSS(clampedPos, leftTime, sizeof(leftTime));
  formatTimeMMSS(durMs, rightTime, sizeof(rightTime));

  u8g2.setDrawColor(1);
  u8g2.setFontMode(1);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 56, leftTime);
  u8g2.drawStr(103, 56, rightTime);

  u8g2.drawFrame(28, 57, 72, 5);
  if (durMs > 0) {
    uint8_t fillW = (uint8_t)(((uint64_t)clampedPos * 70ULL) / (uint64_t)durMs);
    if (fillW > 70) fillW = 70;
    if (fillW > 0) {
      u8g2.drawBox(29, 58, fillW, 3);
    }
  }
}

static void renderFrame(uint32_t nowMs) {
  const char* headerText = DEMO_HEADER;
  const char* lyricText = "";
  uint32_t posMs = 0;
  uint32_t durMs = DEMO_DURATION_MS;
  bool isPlaying = true;
  uint32_t lineStartMs = 0;
  const LyricSegment* segs = nullptr;
  uint8_t segCount = 0;

  if (displayMode == MODE_LIVE_SERIAL) {
    headerText = liveHeader;
    lyricText = liveLyric;
    durMs = liveDurMs;
    isPlaying = livePlaying;
    posMs = livePosMs;
    if (livePlaying) {
      posMs += (nowMs - liveSyncMillis);
    }
    if (durMs > 0 && posMs > durMs) {
      posMs = durMs;
    }
    lineStartMs = liveLineStartMs;
    segs = liveSegs;
    segCount = liveSegCount;
  } else {
    posMs = (nowMs - demoStartMillis) % DEMO_DURATION_MS;
    int16_t idx = selectDemoIndex(posMs);
    lyricText = DEMO_LYRICS[idx].text;
    if (idx != lastDemoIndex) {
      lastDemoIndex = idx;
      demoLineStartMs = DEMO_LYRICS[idx].timeMs;
      uint32_t nextTime = ((size_t)(idx + 1) < DEMO_LYRIC_COUNT)
                              ? DEMO_LYRICS[idx + 1].timeMs
                              : DEMO_DURATION_MS;
      uint32_t gap = (nextTime > demoLineStartMs) ? (nextTime - demoLineStartMs) : 2500UL;
      uint32_t est = 1600UL + (uint32_t)strlen(lyricText) * 85UL;
      uint32_t singDur = (gap > est + 1500UL) ? est : gap;
      buildDemoSegments(lyricText, singDur, demoSegs, demoSegCount);
    }
    lineStartMs = demoLineStartMs;
    segs = demoSegs;
    segCount = demoSegCount;
  }

  u8g2.clearBuffer();
  u8g2.setMaxClipWindow();
  u8g2.setDrawColor(1);
  u8g2.setFontMode(1);

  drawHeader(headerText, isPlaying, nowMs);
  drawLyricArea(lyricText, isPlaying, nowMs, posMs, lineStartMs, segs, segCount);
  drawProgressBar(posMs, durMs);
  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  u8g2.setBusClock(400000);
  u8g2.begin();

  // Kalau OLED pakai alamat 0x3D, buka komentar baris ini:
  // u8g2.setI2CAddress(0x3D * 2);

  u8g2.setContrast(255);
  u8g2.setFontPosTop();
  u8g2.setFontMode(1);
  u8g2.setDrawColor(1);

  demoStartMillis = millis();
  lastFrameMillis = 0;

  Serial.println("READY");
  renderFrame(millis());
}

void loop() {
  uint32_t nowMs = millis();
  pollSerialInput(nowMs);

  if ((nowMs - lastFrameMillis) >= FRAME_INTERVAL_MS) {
    lastFrameMillis = nowMs;
    renderFrame(nowMs);
  }

  delay(2);
}
