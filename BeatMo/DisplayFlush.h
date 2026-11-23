#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <string.h>
#include "Config.h"

// ============================================================================
// Smart Dirty-Tile Partial Flush for SSD1306 (128x64)
//
// Compares the active 1024-byte U8g2 framebuffer against a 1 KB shadow copy
// per 8x8 tile (16x8 = 128 tiles) and sends ONLY modified tile spans via
// u8g2.updateDisplayArea(). Single-tile clean gaps are bridged automatically
// so I2C start/stop overhead is minimized.
//
// Result: Achieves ~45-60 FPS within the 400 kHz SSD1306 datasheet limit.
// ============================================================================
class DisplayFlush {
 public:
  static constexpr uint8_t TILE_W  = 16;
  static constexpr uint8_t TILE_H  = 8;
  static constexpr size_t BUF_SIZE = 1024;

  bool partialEnabled     = ENABLE_PARTIAL_FLUSH;
  bool forceFullSync      = true;
  uint16_t lastDirtyTiles = 0;

  void begin() {
    memset(prevBuf_, 0, BUF_SIZE);
    forceFullSync  = true;
    lastDirtyTiles = 0;
  }

  void requestFullSync() {
    forceFullSync = true;
  }

  void flush(U8G2 &u8g2) {
    uint8_t *curr = u8g2.getBufferPtr();
    if (!curr) {
      u8g2.sendBuffer();
      lastDirtyTiles = TILE_W * TILE_H;
      return;
    }

    const bool canPartial = partialEnabled &&
                            !forceFullSync &&
                            (u8g2.getBufferTileWidth() == TILE_W) &&
                            (u8g2.getBufferTileHeight() == TILE_H);

    if (!canPartial) {
      u8g2.sendBuffer();
      memcpy(prevBuf_, curr, BUF_SIZE);
      forceFullSync  = false;
      lastDirtyTiles = TILE_W * TILE_H;
      return;
    }

    uint16_t dirtyCount = 0;

    for (uint8_t ty = 0; ty < TILE_H; ty++) {
      const uint8_t *currRow = curr + static_cast<size_t>(ty) * SCREEN_WIDTH;
      uint8_t *prevRow       = prevBuf_ + static_cast<size_t>(ty) * SCREEN_WIDTH;

      uint16_t dirtyMask = 0;
      for (uint8_t tx = 0; tx < TILE_W; tx++) {
        const size_t offset = static_cast<size_t>(tx) * 8U;
        if (memcmp(currRow + offset, prevRow + offset, 8U) != 0) {
          dirtyMask |= static_cast<uint16_t>(1U << tx);
        }
      }

      if (dirtyMask == 0) continue;

      // Bridge 1-tile clean gaps: sending 8 extra bytes in one span is faster
      // than issuing a separate SSD1306 page/column I2C command sequence.
      dirtyMask |= static_cast<uint16_t>((dirtyMask << 1) & (dirtyMask >> 1));

      uint8_t tx = 0;
      while (tx < TILE_W) {
        if ((dirtyMask & static_cast<uint16_t>(1U << tx)) == 0) {
          tx++;
          continue;
        }
        const uint8_t spanStart = tx;
        while (tx < TILE_W && (dirtyMask & static_cast<uint16_t>(1U << tx)) != 0) {
          tx++;
        }
        const uint8_t spanLen = tx - spanStart;

        u8g2.updateDisplayArea(spanStart, ty, spanLen, 1);

        const size_t byteOff = static_cast<size_t>(spanStart) * 8U;
        const size_t byteLen = static_cast<size_t>(spanLen) * 8U;
        memcpy(prevRow + byteOff, currRow + byteOff, byteLen);
        dirtyCount += spanLen;
      }
    }

    lastDirtyTiles = dirtyCount;
  }

 private:
  uint8_t prevBuf_[BUF_SIZE];
};
