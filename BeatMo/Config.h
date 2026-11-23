#pragma once

#include <Arduino.h>

// ============================================================================
// Hardware & Display Configuration (ESP32 + 0.96" OLED SSD1306 128x64 I2C)
// ============================================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

// Explicit I2C pins for ESP32
#define I2C_SCL_PIN   22
#define I2C_SDA_PIN   21

// Optional: uncomment if your OLED module uses I2C address 0x3D instead of 0x3C
// #define OLED_I2C_ADDR_0X3D

// Safe default within SSD1306 datasheet specification (400 kHz Fast Mode).
// Combined with Dirty-Tile Partial Flush, this realistically reaches ~45-60 FPS
// without overclocking risks on longer Dupont wires or 4.7k pull-ups.
// You can also test 600/800 kHz live via Serial command: "i2c 600" or "i2c 800".
#define I2C_CLOCK_HZ          400000UL
#define ENABLE_PARTIAL_FLUSH  true

// Serial console speed for live emotion testing & FPS monitoring
#define SERIAL_BAUD           115200UL

// ============================================================================
// Frame Timing & Physics Stability
// ============================================================================
// ~60 FPS ceiling (16,666 us). Delta-time physics guarantees identical motion
// speed whether running at 35 FPS (full-frame) or 60 FPS (dirty-tile partial).
static constexpr unsigned long TARGET_FRAME_US = 16666UL;
static constexpr float MAX_DT_SEC              = 0.05f;

// ============================================================================
// Base Face Geometry (128x64 layout)
// ============================================================================
static constexpr float EYE_LEFT_X        = 40.0f;
static constexpr float EYE_RIGHT_X       = 88.0f;
static constexpr float EYE_BASE_Y        = 27.0f;
static constexpr float MOUTH_BASE_X      = 64.0f;
static constexpr float MOUTH_BASE_Y      = 54.0f;
static constexpr float DEFAULT_ROUNDNESS = 0.42f;

// ============================================================================
// 2nd-Order Spring-Damper Tuning (omega = rad/s, zeta = damping ratio)
//   zeta < 1.0 : soft organic overshoot/bounce
//   zeta = 1.0 : critically damped (no overshoot)
// ============================================================================
static constexpr float SPRING_POS_OMEGA   = 17.5f;
static constexpr float SPRING_POS_ZETA    = 0.78f;

static constexpr float SPRING_SIZE_OMEGA  = 16.0f;
static constexpr float SPRING_SIZE_ZETA   = 0.66f;

static constexpr float SPRING_LID_OMEGA   = 14.5f;
static constexpr float SPRING_LID_ZETA    = 0.86f;

static constexpr float SPRING_MOUTH_OMEGA = 16.5f;
static constexpr float SPRING_MOUTH_ZETA  = 0.72f;

static constexpr float SPRING_SHAPE_OMEGA = 14.0f;
static constexpr float SPRING_SHAPE_ZETA  = 0.82f;
