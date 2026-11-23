/* -------------------------------------------------
 * BeatMo — Expressive Desk-Robot Face for ESP32 + SSD1306 0.96" OLED
 *
 * Fitur:
 *   - 2nd-Order Spring-Damper Physics berbasis delta-time (squash & stretch)
 *   - Dirty-Tile Partial Flush (updateDisplayArea) -> ~45-60 FPS aman di I2C 400 kHz
 *   - 15 Emosi ekspresif dengan kelopak mata dinamis, mulut kurva parametrik,
 *     bentuk mata khusus (hati, spiral, pupil kaget) & efek partikel
 *   - Boot wake-up sequence, idle breathing, double-blink, 3D gaze perspective
 *   - Mode otomatis natural + kontrol Serial Monitor (115200 baud, ketik 'help')
 * -------------------------------------------------*/

#include <U8g2lib.h>
#include <Wire.h>
#include "Config.h"
#include "DisplayFlush.h"
#include "FaceController.h"
#include "FaceRenderer.h"

// SSD1306 128x64 Hardware I2C (Full Framebuffer Mode)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0, /* reset=*/U8X8_PIN_NONE, /* clock=*/I2C_SCL_PIN, /* data=*/I2C_SDA_PIN);

DisplayFlush displayFlush;
FaceController face;

unsigned long lastFrameUs  = 0;
unsigned long fpsWindowMs  = 0;
uint32_t fpsFrameCount     = 0;
uint32_t fpsDirtyTileSum   = 0;
uint32_t currentI2CClockHz = I2C_CLOCK_HZ;

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  // Seed PRNG from ESP32 hardware RNG so animations vary on every boot
#if defined(ESP32)
  randomSeed(esp_random());
#else
  randomSeed(analogRead(0));
#endif

  // Configure U8g2 bus clock BEFORE begin() so U8g2 applies it on every transfer
  u8g2.setBusClock(currentI2CClockHz);
#ifdef OLED_I2C_ADDR_0X3D
  u8g2.setI2CAddress(0x3D * 2);
#endif
  u8g2.begin();

  displayFlush.begin();
  face.begin();

  lastFrameUs = micros();
  fpsWindowMs = millis();

  Serial.println(F("\nBeatMo ready! Ketik 'help' di Serial Monitor untuk daftar perintah."));
}

void loop() {
  // 1. Handle non-blocking Serial commands
  face.pollSerial();

  if (face.cmdTogglePartial) {
    face.cmdTogglePartial       = false;
    displayFlush.partialEnabled = face.cmdPartialValue;
    displayFlush.requestFullSync();
  }
  if (face.cmdRequestRefresh) {
    face.cmdRequestRefresh = false;
    displayFlush.requestFullSync();
  }
  if (face.cmdChangeI2C) {
    face.cmdChangeI2C = false;
    currentI2CClockHz = face.cmdNewI2CHz;
    u8g2.setBusClock(currentI2CClockHz);
    displayFlush.requestFullSync();
  }

  // 2. Frame-paced update & render with true delta-time
  const unsigned long nowUs     = micros();
  const unsigned long elapsedUs = nowUs - lastFrameUs;

  if (elapsedUs >= TARGET_FRAME_US) {
    lastFrameUs    = nowUs;
    const float dt = static_cast<float>(elapsedUs) * 1e-6f;

    face.update(dt);

    u8g2.clearBuffer();
    FaceRenderer::drawEye(u8g2, face.leftEye);
    FaceRenderer::drawEye(u8g2, face.rightEye);
    FaceRenderer::drawMouth(u8g2, face.mouth);
    FaceRenderer::drawEffects(u8g2, face.fx);
    displayFlush.flush(u8g2);

    fpsFrameCount++;
    fpsDirtyTileSum += displayFlush.lastDirtyTiles;
  }

  // 3. Optional 1-second FPS & Dirty-Tile Telemetry (toggle via Serial 'fps')
  const unsigned long nowMs = millis();
  if (nowMs - fpsWindowMs >= 1000UL) {
    if (face.fpsReportEnabled && fpsFrameCount > 0) {
      const float elapsedSec = static_cast<float>(nowMs - fpsWindowMs) * 0.001f;
      const float fps        = static_cast<float>(fpsFrameCount) / elapsedSec;
      const float avgTiles   = static_cast<float>(fpsDirtyTileSum) / static_cast<float>(fpsFrameCount);
      const float areaPct    = (avgTiles / 128.0f) * 100.0f;

      Serial.print(F("[FPS] "));
      Serial.print(fps, 1);
      Serial.print(F(" fps | dirty: "));
      Serial.print(avgTiles, 1);
      Serial.print(F("/128 tiles ("));
      Serial.print(areaPct, 0);
      Serial.print(F("%) | partial: "));
      Serial.print(displayFlush.partialEnabled ? F("ON") : F("OFF"));
      Serial.print(F(" | I2C: "));
      Serial.print(currentI2CClockHz / 1000UL);
      Serial.print(F(" kHz | emo: "));
      Serial.println(EMOTION_NAMES[face.currentEmotion]);
    }
    fpsWindowMs     = nowMs;
    fpsFrameCount   = 0;
    fpsDirtyTileSum = 0;
  }
}
