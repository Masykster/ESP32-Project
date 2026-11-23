# BeatMo

**BeatMo** (*Beat + Emo/Mochi*) is an expressive desk-robot OLED face for **ESP32 + 0.96" SSD1306 (128x64 I2C)** featuring delta-time 2nd-order spring physics, ~45–60 FPS dirty-tile rendering at a datasheet-safe 400 kHz I2C clock, 15 dynamic emotions, and real-time music + synced-lyric emotion tracking from Windows.

---

## Features

- **Organic Spring-Damper Physics (`Spring.h`)**: 2nd-order harmonic motion with *delta-time* integration, *sub-stepping*, *squash-and-stretch*, and anti-jitter deadzone snapping.
- **Dirty-Tile Partial Flush (`DisplayFlush.h`)**: Diffs the 1 KB framebuffer per 8×8 tile and sends only changed regions via `u8g2.updateDisplayArea()`, reaching ~45–60 FPS without overclocking the 400 kHz SSD1306 I2C bus.
- **15 Expressive Emotions**: `neutral`, `happy`, `sad`, `surprised`, `sleepy`, `angry`, `love`, `laugh`, `cry`, `wink`, `confused`, `shy`, `dizzy`, `suspicious`, and `scared` — complete with dynamic eyelid masking, parametric curved mouths, custom eyes (hearts, rotating spirals, trembling pupils), and particle FX (`Zzz`, falling tears, floating hearts, blush, sparkles, anger vein, `?`, sweat drop).
- **Lifelike Autonomy**: Boot wake-up choreography, breathing idle motion, single/double/slow blinks, 3D gaze perspective, and non-repeating emotion shuffling.
- **Music & Lyric Emotion Sync (`music_sync.py`)**:
  - Reads active track & playback position via Windows SMTC and fetches synced lyrics (`lyric_bridge.py` integration).
  - Captures real-time Windows system audio via WASAPI Loopback (`pyaudiowpatch`, with `sounddevice` fallback) using NumPy FFT (bass, vocal, treble, beat detection).
  - Streams compact 25 Hz packets over USB Serial so BeatMo changes emotion with the lyrics/music, head-bobs on bass hits, and lip-syncs to vocals.

---

## Wiring (ESP32 ↔ SSD1306 128x64 I2C)

| SSD1306 Pin | ESP32 Pin |
| --- | --- |
| `VCC` | `3.3V` |
| `GND` | `GND` |
| `SCL` | `GPIO 22` |
| `SDA` | `GPIO 21` |

*(If your OLED uses I2C address `0x3D` instead of `0x3C`, uncomment `#define OLED_I2C_ADDR_0X3D` in `Config.h`.)*

---

## Quick Start

### 1. Upload ESP32 Firmware
1. Install the **`U8g2`** library in Arduino IDE (Library Manager).
2. Open `BeatMo/BeatMo.ino` in Arduino IDE.
3. Select your **ESP32** board and COM port, then click **Upload**.

### 2. Serial Monitor Control (`115200` baud)
Open Serial Monitor and type `help` to see all interactive commands:
- Lock an emotion by name (`happy`, `love`, `cry`, `angry`, `dizzy`, etc.) or index (`0`–`14`).
- `auto` — return to autonomous lifelike mode.
- `next` — cycle to the next emotion.
- `blink`, `dblink`, `qwink`, `look`, `boot` — trigger animations on demand.
- `fps` — toggle real-time FPS & dirty-tile telemetry.
- `partial on` / `partial off` / `i2c 400|600|800` — benchmark rendering settings live.

### 3. Run PC Music & Lyric Sync (Optional)
> Make sure the Arduino IDE Serial Monitor (and any other app using the ESP32 COM port) is closed first.

```powershell
pip install -r requirements_music.txt
python music_sync.py
```

Useful flags:
- `--dry-run` — preview track, lyrics, audio levels, and predicted emotion without connecting an ESP32.
- `--port COM6` — specify the COM port manually (default: `auto`).
- `--list-devices` — list detected audio input & WASAPI loopback devices.
- `--bitchord-only` — follow only BitChord Desktop SMTC sessions.
