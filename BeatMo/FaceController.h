#pragma once

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "Config.h"
#include "FaceTypes.h"

// ============================================================================
// FaceController: Emotion Targets, Boot Sequence, Natural Blink & Double-Blink,
// Gaze Saccades with 3D Perspective, Idle Breathing, Particles & Serial Control
// ============================================================================
class FaceController {
 public:
  AnimatedEye leftEye;
  AnimatedEye rightEye;
  AnimatedMouth mouth;
  FaceFX fx;
  MusicState music;

  Emotion currentEmotion = EMO_NEUTRAL;
  bool autoMode          = true;
  bool fpsReportEnabled  = false;

  // Flags polled by main .ino for DisplayFlush / U8g2 hardware commands
  bool cmdTogglePartial  = false;
  bool cmdPartialValue   = ENABLE_PARTIAL_FLUSH;
  bool cmdRequestRefresh = false;
  bool cmdChangeI2C      = false;
  uint32_t cmdNewI2CHz   = I2C_CLOCK_HZ;

  void begin() {
    leftEye.isLeft  = true;
    rightEye.isLeft = false;

    leftEye.configureSprings();
    rightEye.configureSprings();
    mouth.configureSprings();
    gazeX_.configure(SPRING_POS_OMEGA, 0.80f);
    gazeY_.configure(SPRING_POS_OMEGA, 0.80f);
    gazeX_.snap(0.0f);
    gazeY_.snap(0.0f);

    refillShuffleBag_();
    startBootSequence();
  }

  void startBootSequence() {
    bootActive_ = true;
    bootStage_  = 0;
    bootTimer_  = 0.0f;

    // Start from dark horizontal sleep slits
    applyEmotionTargets_(EMO_NEUTRAL, false);
    leftEye.h.snap(2.0f);
    rightEye.h.snap(2.0f);
    leftEye.w.snap(26.0f);
    rightEye.w.snap(26.0f);
    leftEye.x.snap(EYE_LEFT_X);
    rightEye.x.snap(EYE_RIGHT_X);
    leftEye.y.snap(EYE_BASE_Y + 4.0f);
    rightEye.y.snap(EYE_BASE_Y + 4.0f);
    leftEye.topInner.snap(0.0f);
    leftEye.topOuter.snap(0.0f);
    leftEye.bottomLid.snap(0.0f);
    rightEye.topInner.snap(0.0f);
    rightEye.topOuter.snap(0.0f);
    rightEye.bottomLid.snap(0.0f);
    mouth.snapAll();
    mouth.w.snap(6.0f);

    // Keep eyes closed during initial brief sleep pause
    leftEye.h.set(2.0f);
    rightEye.h.set(2.0f);
    gazeX_.snap(0.0f);
    gazeY_.snap(0.0f);
    resetFX_();
  }

  void setEmotion(Emotion emo, bool resetGaze = true) {
    if (emo >= EMOTION_COUNT) emo = EMO_NEUTRAL;
    bootActive_ = false;
    applyEmotionTargets_(emo, resetGaze);
  }

  void triggerBlink(bool doubleBlink = false, bool slow = false) {
    if (blinkState_ != BLINK_IDLE) return;
    if (!leftEye.canBlink && !rightEye.canBlink) return;
    blinkState_       = BLINK_CLOSING;
    blinkTimer_       = 0.0f;
    blinkSlow_        = slow;
    blinkWinkRight_   = false;
    pendingDblBlink_  = doubleBlink;
  }

  void triggerWink() {
    if (blinkState_ != BLINK_IDLE) return;
    if (!rightEye.canBlink) return;
    blinkState_      = BLINK_CLOSING;
    blinkTimer_      = 0.0f;
    blinkSlow_       = false;
    blinkWinkRight_  = true;
    pendingDblBlink_ = false;
  }

  void triggerSaccade() {
    const float gx = static_cast<float>(random(-12, 13));
    const float gy = static_cast<float>(random(-7, 8));
    gazeX_.set(gx);
    gazeY_.set(gy);
  }

  void update(float dt) {
    if (dt <= 0.0f) return;
    if (dt > MAX_DT_SEC) dt = MAX_DT_SEC;

    lifeTime_    += dt;
    emotionTime_ += dt;

    if (bootActive_) {
      updateBoot_(dt);
    } else {
      updateAutonomous_(dt);
    }

    updateBlink_(dt);

    // Advance springs
    gazeX_.update(dt);
    gazeY_.update(dt);
    leftEye.update(dt);
    rightEye.update(dt);
    mouth.update(dt);

    // Apply continuous breathing, 3D gaze perspective, and per-emotion micro-motion
    applyLiveMotion_(dt);
    updateParticles_(dt);
  }

  // Non-blocking Serial Monitor command reader
  void pollSerial() {
    while (Serial.available() > 0) {
      const char ch = static_cast<char>(Serial.read());
      if (ch == '\n' || ch == '\r') {
        if (cmdLen_ > 0) {
          cmdBuf_[cmdLen_] = '\0';
          processCommand_(cmdBuf_);
          cmdLen_ = 0;
        }
      } else if (cmdLen_ < sizeof(cmdBuf_) - 1) {
        cmdBuf_[cmdLen_++] = ch;
      }
    }
  }

  void printHelp() const {
    Serial.println(F("\n=== BeatMo Commands (115200 baud) ==="));
    Serial.println(F("Emotions (ketik nama atau 0-14 untuk mengunci):"));
    for (uint8_t i = 0; i < EMOTION_COUNT; i++) {
      Serial.print(F("  "));
      if (i < 10) Serial.print(' ');
      Serial.print(i);
      Serial.print(F(" : "));
      Serial.println(EMOTION_NAMES[i]);
    }
    Serial.println(F("Actions:"));
    Serial.println(F("  auto          -> kembali ke mode otomatis"));
    Serial.println(F("  next          -> pindah ke emosi berikutnya"));
    Serial.println(F("  blink/dblink  -> kedip / kedip ganda"));
    Serial.println(F("  qwink / look  -> kedip sebelah singkat / lirik acak"));
    Serial.println(F("  boot          -> ulang animasi bangun"));
    Serial.println(F("  fps           -> toggle info FPS & dirty tiles"));
    Serial.println(F("  partial on/off-> bandingkan partial vs full-frame"));
    Serial.println(F("  i2c 400|600|800 -> ubah clock I2C runtime (kHz)"));
    Serial.println(F("  refresh       -> paksa resync full-frame"));
    Serial.println(F("  music off     -> keluar dari mode sinkronisasi musik"));
    Serial.println(F("  M,e,E,B,V,b   -> paket musik real-time dari PC bridge"));
    Serial.println(F("========================================\n"));
  }

 private:
  enum BlinkState : uint8_t {
    BLINK_IDLE = 0,
    BLINK_CLOSING,
    BLINK_HOLD,
    BLINK_OPENING,
    BLINK_DBL_GAP
  };

  SpringVal gazeX_, gazeY_;

  float lifeTime_    = 0.0f;
  float emotionTime_ = 0.0f;

  // Boot sequence state
  bool bootActive_   = true;
  uint8_t bootStage_ = 0;
  float bootTimer_   = 0.0f;

  // Blink engine state
  BlinkState blinkState_  = BLINK_IDLE;
  float blinkTimer_       = 0.0f;
  float nextBlinkIn_      = 2.6f;
  bool blinkSlow_         = false;
  bool blinkWinkRight_    = false;
  bool pendingDblBlink_   = false;

  // Autonomous director state
  float nextActionIn_    = 2.5f;
  float nextEmotionIn_   = 4.2f;
  bool returnToNeutral_  = false;
  uint8_t shuffleBag_[EMOTION_COUNT - 1];
  uint8_t shuffleIdx_    = EMOTION_COUNT - 1;

  // Serial input buffer
  char cmdBuf_[48];
  size_t cmdLen_ = 0;

  void refillShuffleBag_() {
    const uint8_t count = EMOTION_COUNT - 1;
    for (uint8_t i = 0; i < count; i++) {
      shuffleBag_[i] = i + 1;  // all non-neutral emotions (1..14)
    }
    // Fisher-Yates shuffle
    for (uint8_t i = count - 1; i > 0; i--) {
      const uint8_t j = static_cast<uint8_t>(random(0, i + 1));
      const uint8_t tmp = shuffleBag_[i];
      shuffleBag_[i]    = shuffleBag_[j];
      shuffleBag_[j]    = tmp;
    }
    shuffleIdx_ = 0;
  }

  Emotion pickNextAutoEmotion_() {
    // Periodically pause briefly in NEUTRAL so rhythm feels lifelike
    if (currentEmotion != EMO_NEUTRAL && returnToNeutral_) {
      returnToNeutral_ = false;
      return EMO_NEUTRAL;
    }
    returnToNeutral_ = (random(100) < 35);

    if (shuffleIdx_ >= (EMOTION_COUNT - 1)) {
      refillShuffleBag_();
    }
    Emotion next = static_cast<Emotion>(shuffleBag_[shuffleIdx_++]);
    if (next == currentEmotion) {
      if (shuffleIdx_ >= (EMOTION_COUNT - 1)) refillShuffleBag_();
      next = static_cast<Emotion>(shuffleBag_[shuffleIdx_++]);
    }
    return next;
  }

  void resetFX_() {
    fx.floaterMode   = FLOATER_NONE;
    fx.showBlush     = false;
    fx.showAngerVein = false;
    fx.showQuestion  = false;
    fx.showSweat     = false;
    fx.showTears     = false;
    fx.fxTime        = 0.0f;
    fx.sweatPhase    = 0.0f;
    for (int i = 0; i < 2; i++) {
      fx.tears[i].active       = false;
      fx.tears[i].respawnDelay = (i == 0) ? 0.05f : 0.45f;
    }
  }

  void initFloaters_(FloaterMode mode) {
    fx.floaterMode = mode;
    for (int i = 0; i < 3; i++) {
      fx.floaters[i].slot  = static_cast<uint8_t>(i);
      fx.floaters[i].phase = static_cast<float>(i) * 0.33f;
      if (mode == FLOATER_ZZZ) {
        fx.floaters[i].speed = 0.34f;
      } else if (mode == FLOATER_HEARTS) {
        fx.floaters[i].speed = 0.48f;
      } else if (mode == FLOATER_SPARKLES) {
        fx.floaters[i].speed = 0.95f;
      }
    }
  }

  void setEyes_(float lx, float ly, float lw, float lh,
                float rx, float ry, float rw, float rh,
                float round = DEFAULT_ROUNDNESS) {
    leftEye.x.set(lx);
    leftEye.y.set(ly);
    leftEye.w.set(lw);
    leftEye.h.set(lh);
    leftEye.roundness.set(round);

    rightEye.x.set(rx);
    rightEye.y.set(ry);
    rightEye.w.set(rw);
    rightEye.h.set(rh);
    rightEye.roundness.set(round);
  }

  void setLids_(float lTopIn, float lTopOut, float lBot,
                float rTopIn, float rTopOut, float rBot) {
    leftEye.topInner.set(lTopIn);
    leftEye.topOuter.set(lTopOut);
    leftEye.bottomLid.set(lBot);

    rightEye.topInner.set(rTopIn);
    rightEye.topOuter.set(rTopOut);
    rightEye.bottomLid.set(rBot);
  }

  void setMouth_(float x, float y, float w, float h,
                 float curve, float open, MouthStyle style = MOUTH_CURVE) {
    mouth.x.set(x);
    mouth.y.set(y);
    mouth.w.set(w);
    mouth.h.set(h);
    mouth.curve.set(curve);
    mouth.open.set(open);
    mouth.style = style;
  }

  void applyEmotionTargets_(Emotion emo, bool resetGaze) {
    currentEmotion = emo;
    emotionTime_   = 0.0f;

    if (resetGaze) {
      gazeX_.set(0.0f);
      gazeY_.set(0.0f);
    }

    // Defaults
    leftEye.style    = EYE_NORMAL;
    rightEye.style   = EYE_NORMAL;
    leftEye.canBlink = true;
    rightEye.canBlink = true;
    resetFX_();

    const float lx = EYE_LEFT_X;
    const float rx = EYE_RIGHT_X;
    const float cy = EYE_BASE_Y;
    const float mx = MOUTH_BASE_X;
    const float my = MOUTH_BASE_Y;

    switch (emo) {
      case EMO_NEUTRAL:
        setEyes_(lx, cy, 26, 34, rx, cy, 26, 34, 0.42f);
        setLids_(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        setMouth_(mx, my, 18, 6, 0.25f, 0.0f, MOUTH_CURVE);
        break;

      case EMO_HAPPY:
        setEyes_(lx, cy - 2, 28, 28, rx, cy - 2, 28, 28, 0.45f);
        setLids_(0.0f, 0.0f, 0.68f, 0.0f, 0.0f, 0.68f);
        setMouth_(mx, my - 1, 28, 9, 0.95f, 0.0f, MOUTH_CURVE);
        initFloaters_(FLOATER_SPARKLES);
        break;

      case EMO_SAD:
        setEyes_(lx + 2, cy + 5, 24, 28, rx - 2, cy + 5, 24, 28, 0.42f);
        setLids_(0.0f, 0.46f, 0.0f, 0.0f, 0.46f, 0.0f);
        setMouth_(mx, my + 2, 18, 8, -0.82f, 0.0f, MOUTH_CURVE);
        break;

      case EMO_SURPRISED:
        setEyes_(lx, cy - 4, 24, 40, rx, cy - 4, 24, 40, 0.48f);
        setLids_(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        setMouth_(mx, my, 13, 13, 0.0f, 1.0f, MOUTH_OPEN_O);
        // Snappy pop-open impulse!
        leftEye.h.impulse(95.0f);
        rightEye.h.impulse(95.0f);
        mouth.h.impulse(55.0f);
        break;

      case EMO_SLEEPY:
        setEyes_(lx, cy + 4, 26, 26, rx, cy + 4, 26, 26, 0.42f);
        setLids_(0.64f, 0.64f, 0.0f, 0.64f, 0.64f, 0.0f);
        setMouth_(mx, my + 1, 10, 5, -0.10f, 0.0f, MOUTH_CURVE);
        initFloaters_(FLOATER_ZZZ);
        break;

      case EMO_ANGRY:
        setEyes_(lx + 4, cy + 2, 26, 28, rx - 4, cy + 2, 26, 28, 0.34f);
        setLids_(0.54f, 0.0f, 0.08f, 0.54f, 0.0f, 0.08f);
        setMouth_(mx, my + 1, 22, 7, -0.65f, 0.0f, MOUTH_CURVE);
        fx.showAngerVein = true;
        break;

      case EMO_LOVE:
        setEyes_(lx, cy - 1, 28, 28, rx, cy - 1, 28, 28, 0.45f);
        setLids_(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        setMouth_(mx, my - 1, 24, 8, 0.85f, 0.0f, MOUTH_CURVE);
        leftEye.style     = EYE_HEART;
        rightEye.style    = EYE_HEART;
        leftEye.canBlink  = false;
        rightEye.canBlink = false;
        fx.showBlush      = true;
        initFloaters_(FLOATER_HEARTS);
        break;

      case EMO_LAUGH:
        setEyes_(lx, cy - 3, 30, 24, rx, cy - 3, 30, 24, 0.46f);
        setLids_(0.0f, 0.0f, 0.78f, 0.0f, 0.0f, 0.78f);
        setMouth_(mx, my - 2, 32, 10, 0.90f, 0.68f, MOUTH_CURVE);
        leftEye.canBlink  = false;
        rightEye.canBlink = false;
        fx.showBlush      = true;
        break;

      case EMO_CRY:
        setEyes_(lx + 3, cy + 4, 24, 26, rx - 3, cy + 4, 24, 26, 0.40f);
        setLids_(0.0f, 0.52f, 0.14f, 0.0f, 0.52f, 0.14f);
        setMouth_(mx, my + 1, 20, 8, -0.90f, 0.0f, MOUTH_CURVE);
        fx.showTears = true;
        break;

      case EMO_WINK:
        setEyes_(lx, cy - 1, 26, 34, rx, cy, 28, 26, 0.44f);
        setLids_(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.78f);
        setMouth_(mx + 3, my - 1, 24, 8, 0.85f, 0.0f, MOUTH_CURVE);
        rightEye.canBlink = false;
        rightEye.h.impulse(-60.0f);
        break;

      case EMO_CONFUSED:
        // Asymmetric curious eyes (left wide & high, right smaller & slightly squinted)
        setEyes_(lx, cy - 3, 28, 36, rx, cy + 3, 22, 22, 0.44f);
        setLids_(0.0f, 0.0f, 0.0f, 0.28f, 0.0f, 0.0f);
        setMouth_(mx + 2, my + 1, 14, 5, -0.25f, 0.0f, MOUTH_CURVE);
        fx.showQuestion = true;
        break;

      case EMO_SHY:
        setEyes_(lx, cy + 3, 24, 30, rx, cy + 3, 24, 30, 0.45f);
        setLids_(0.0f, 0.14f, 0.28f, 0.0f, 0.14f, 0.28f);
        setMouth_(mx, my, 18, 7, 0.3f, 0.0f, MOUTH_CAT);
        gazeX_.set(7.0f);
        gazeY_.set(3.5f);
        fx.showBlush = true;
        break;

      case EMO_DIZZY:
        setEyes_(lx, cy, 28, 28, rx, cy, 28, 28, 0.48f);
        setLids_(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        setMouth_(mx, my, 24, 8, 0.0f, 0.0f, MOUTH_WAVY);
        leftEye.style     = EYE_DIZZY;
        rightEye.style    = EYE_DIZZY;
        leftEye.canBlink  = false;
        rightEye.canBlink = false;
        break;

      case EMO_SUSPICIOUS:
        // Narrow skeptical stare with slight asymmetry
        setEyes_(lx, cy + 2, 28, 26, rx, cy + 1, 28, 28, 0.36f);
        setLids_(0.50f, 0.56f, 0.0f, 0.40f, 0.48f, 0.0f);
        setMouth_(mx + 2, my + 1, 18, 4, -0.10f, 0.0f, MOUTH_CURVE);
        break;

      case EMO_SCARED:
        setEyes_(lx - 2, cy - 3, 26, 38, rx + 2, cy - 3, 26, 38, 0.46f);
        setLids_(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        setMouth_(mx, my + 1, 20, 8, -0.4f, 0.0f, MOUTH_WAVY);
        leftEye.style  = EYE_SCARED_PUPIL;
        rightEye.style = EYE_SCARED_PUPIL;
        fx.showSweat   = true;
        leftEye.h.impulse(70.0f);
        rightEye.h.impulse(70.0f);
        break;

      default:
        break;
    }
  }

  // --- Boot Wake-Up Choreography (~2.8 seconds) ---
  void updateBoot_(float dt) {
    bootTimer_ += dt;
    switch (bootStage_) {
      case 0:  // Brief sleep pause -> slowly open eyes
        if (bootTimer_ >= 0.35f) {
          bootStage_ = 1;
          bootTimer_ = 0.0f;
          leftEye.h.set(34.0f);
          rightEye.h.set(34.0f);
          leftEye.y.set(EYE_BASE_Y);
          rightEye.y.set(EYE_BASE_Y);
          mouth.w.set(18.0f);
          leftEye.h.impulse(40.0f);
          rightEye.h.impulse(40.0f);
        }
        break;
      case 1:  // Blink once after waking
        if (bootTimer_ >= 0.65f) {
          bootStage_ = 2;
          bootTimer_ = 0.0f;
          triggerBlink(false, false);
        }
        break;
      case 2:  // Look left
        if (bootTimer_ >= 0.35f) {
          bootStage_ = 3;
          bootTimer_ = 0.0f;
          gazeX_.set(-10.0f);
          gazeY_.set(-2.0f);
        }
        break;
      case 3:  // Look right
        if (bootTimer_ >= 0.45f) {
          bootStage_ = 4;
          bootTimer_ = 0.0f;
          gazeX_.set(10.0f);
          gazeY_.set(-1.0f);
        }
        break;
      case 4:  // Center + brief happy greeting
        if (bootTimer_ >= 0.45f) {
          bootStage_ = 5;
          bootTimer_ = 0.0f;
          applyEmotionTargets_(EMO_HAPPY, true);
        }
        break;
      case 5:  // Settle into NEUTRAL and hand off to autonomous director
        if (bootTimer_ >= 0.90f) {
          bootActive_    = false;
          applyEmotionTargets_(EMO_NEUTRAL, true);
          nextActionIn_  = 1.8f;
          nextEmotionIn_ = 3.2f;
        }
        break;
    }
  }

  // --- Autonomous Behavior Director ---
  void updateAutonomous_(float dt) {
    // Spontaneous blinking (works in both auto and locked manual modes!)
    if (blinkState_ == BLINK_IDLE) {
      nextBlinkIn_ -= dt;
      if (nextBlinkIn_ <= 0.0f) {
        const bool isSleepy = (currentEmotion == EMO_SLEEPY);
        const bool doDouble = !isSleepy && (random(100) < 20);
        triggerBlink(doDouble, isSleepy);
        nextBlinkIn_ = isSleepy
                           ? static_cast<float>(random(3200, 5800)) * 0.001f
                           : static_cast<float>(random(2000, 5200)) * 0.001f;
      }
    }

    // Music watchdog: auto-resume autonomous behavior if PC bridge stops > 5s
    if (music.active) {
      music.sincePacketS += dt;
      if (music.sincePacketS > 5.0f) {
        music.active      = false;
        music.energy      = 0.0f;
        music.bass        = 0.0f;
        music.vocal       = 0.0f;
        music.smoothVocal = 0.0f;
        music.beatPulse   = 0.0f;
        autoMode          = true;
        nextEmotionIn_    = 2.0f;
        applyEmotionTargets_(EMO_NEUTRAL, true);
      }
      return;
    }

    if (!autoMode) return;

    // Periodic subtle eye saccades (skip emotions with scripted gaze)
    nextActionIn_ -= dt;
    if (nextActionIn_ <= 0.0f) {
      nextActionIn_ = static_cast<float>(random(1100, 2600)) * 0.001f;
      const bool scriptedGaze = (currentEmotion == EMO_SHY ||
                                 currentEmotion == EMO_SUSPICIOUS ||
                                 currentEmotion == EMO_DIZZY ||
                                 currentEmotion == EMO_SLEEPY);
      if (!scriptedGaze) {
        if (random(100) < 68) {
          triggerSaccade();
        } else {
          gazeX_.set(0.0f);
          gazeY_.set(0.0f);
        }
      }
    }

    // Emotion transition timer
    nextEmotionIn_ -= dt;
    if (nextEmotionIn_ <= 0.0f) {
      Emotion next = pickNextAutoEmotion_();
      applyEmotionTargets_(next, true);
      nextEmotionIn_ = (next == EMO_NEUTRAL)
                           ? static_cast<float>(random(2200, 3400)) * 0.001f
                           : static_cast<float>(random(3600, 5800)) * 0.001f;
    }
  }

  // --- 3-Phase Natural Blink + Double-Blink Engine ---
  void updateBlink_(float dt) {
    leftEye.blinkClose  = 0.0f;
    rightEye.blinkClose = 0.0f;

    if (blinkState_ == BLINK_IDLE) return;

    blinkTimer_ += dt;
    const float closeDur = blinkSlow_ ? 0.14f : 0.055f;
    const float holdDur  = blinkSlow_ ? 0.09f : 0.032f;
    const float openDur  = blinkSlow_ ? 0.17f : 0.078f;

    float amt = 0.0f;
    switch (blinkState_) {
      case BLINK_CLOSING: {
        float t = clampf(blinkTimer_ / closeDur, 0.0f, 1.0f);
        // Smooth ease-in-out cubic
        amt = t * t * (3.0f - 2.0f * t);
        if (t >= 1.0f) {
          blinkState_ = BLINK_HOLD;
          blinkTimer_ = 0.0f;
        }
        break;
      }
      case BLINK_HOLD:
        amt = 1.0f;
        if (blinkTimer_ >= holdDur) {
          blinkState_ = BLINK_OPENING;
          blinkTimer_ = 0.0f;
        }
        break;
      case BLINK_OPENING: {
        float t = clampf(blinkTimer_ / openDur, 0.0f, 1.0f);
        amt = 1.0f - (t * t * (3.0f - 2.0f * t));
        if (t >= 1.0f) {
          amt = 0.0f;
          // Crisp upward pop impulse when eye re-opens
          if (!blinkWinkRight_ && leftEye.canBlink) leftEye.h.impulse(38.0f);
          if (rightEye.canBlink) rightEye.h.impulse(38.0f);

          if (pendingDblBlink_) {
            pendingDblBlink_ = false;
            blinkState_      = BLINK_DBL_GAP;
            blinkTimer_      = 0.0f;
          } else {
            blinkState_ = BLINK_IDLE;
          }
        }
        break;
      }
      case BLINK_DBL_GAP:
        amt = 0.0f;
        if (blinkTimer_ >= 0.105f) {
          blinkState_ = BLINK_CLOSING;
          blinkTimer_ = 0.0f;
        }
        break;
      default:
        break;
    }

    if (!blinkWinkRight_ && leftEye.canBlink) {
      leftEye.blinkClose = amt;
    }
    if (rightEye.canBlink) {
      rightEye.blinkClose = amt;
    }
  }

  // --- Continuous Living Motion (Breathing, 3D Gaze, Emotion Loops) ---
  void applyLiveMotion_(float dt) {
    // 1. Base sinusoidal breathing (slower & deeper when sleepy)
    const float breathFreq = (currentEmotion == EMO_SLEEPY) ? 1.15f : 2.05f;
    const float breathAmp  = (currentEmotion == EMO_SLEEPY) ? 1.75f : 1.05f;
    const float breathY    = sinf(lifeTime_ * breathFreq) * breathAmp;
    const float breathSc   = 1.0f + sinf(lifeTime_ * breathFreq) * 0.018f;

    // 2. Gaze with 3D spherical perspective width scaling
    const float gx    = gazeX_.val;
    const float gy    = gazeY_.val;
    const float gNorm = clampf(gx / 12.0f, -1.0f, 1.0f);

    leftEye.renderOx      = gx;
    leftEye.renderOy      = gy + breathY;
    leftEye.renderScaleW  = 1.0f + gNorm * 0.06f;
    leftEye.renderScaleH  = breathSc;

    rightEye.renderOx     = gx;
    rightEye.renderOy     = gy + breathY;
    rightEye.renderScaleW = 1.0f - gNorm * 0.06f;
    rightEye.renderScaleH = breathSc;

    mouth.renderOx        = gx * 0.45f;
    mouth.renderOy        = gy * 0.18f + breathY * 0.55f;
    mouth.renderOpenBoost = 0.0f;

    leftEye.stylePhase   += dt;
    rightEye.stylePhase  += dt;
    mouth.wavePhase      += dt * 8.5f;

    // 2b. Music-reactive beat bounce & vocal lip-sync
    if (music.active) {
      music.beatPulse   *= expf(-dt * 12.0f);
      music.smoothVocal += (music.vocal - music.smoothVocal) * clampf(dt * 18.0f, 0.0f, 1.0f);

      const float headBob   = music.beatPulse * 2.8f + sinf(lifeTime_ * 7.5f) * music.energy * 0.5f;
      const float beatWide  = 1.0f + music.beatPulse * 0.075f;
      const float beatSquash = 1.0f - music.beatPulse * 0.045f;

      leftEye.renderOy      += headBob;
      rightEye.renderOy     += headBob;
      leftEye.renderScaleW  *= beatWide;
      rightEye.renderScaleW *= beatWide;
      leftEye.renderScaleH  *= beatSquash;
      rightEye.renderScaleH *= beatSquash;

      mouth.renderOy += headBob * 0.65f;
      if (mouth.style == MOUTH_CURVE && music.smoothVocal > 0.07f) {
        const float singOpen = clampf((music.smoothVocal - 0.07f) * 0.85f + music.beatPulse * 0.18f,
                                      0.0f, 0.70f);
        mouth.renderOpenBoost = singOpen;
      }
    }

    // 3. Per-emotion live micro-animations
    const float t = emotionTime_;
    switch (currentEmotion) {
      case EMO_HAPPY: {
        const float hop = -fabsf(sinf(t * 3.8f)) * 1.8f;
        leftEye.renderOy  += hop;
        rightEye.renderOy += hop;
        mouth.renderOy    += hop * 0.45f;
        break;
      }
      case EMO_ANGRY: {
        const float tx = sinf(t * 48.0f) * 0.85f;
        const float ty = cosf(t * 53.0f) * 0.55f;
        leftEye.renderOx  += tx;
        rightEye.renderOx += tx;
        leftEye.renderOy  += ty;
        rightEye.renderOy += ty;
        mouth.renderOx    += tx * 0.6f;
        break;
      }
      case EMO_LOVE: {
        // Double-peak lub-dub heartbeat curve (~1.25 Hz)
        const float cyc = fmodf(t * 1.25f, 1.0f);
        float beat = 0.0f;
        if (cyc < 0.14f) {
          beat = sinf((cyc / 0.14f) * 3.14159f) * 0.16f;
        } else if (cyc > 0.20f && cyc < 0.34f) {
          beat = sinf(((cyc - 0.20f) / 0.14f) * 3.14159f) * 0.10f;
        }
        leftEye.renderScaleW  *= (1.0f + beat);
        leftEye.renderScaleH  *= (1.0f + beat);
        rightEye.renderScaleW *= (1.0f + beat);
        rightEye.renderScaleH *= (1.0f + beat);
        break;
      }
      case EMO_LAUGH: {
        const float bounce = sinf(t * 15.0f);
        const float eyeHop = -fabsf(bounce) * 2.6f;
        leftEye.renderOy      += eyeHop;
        rightEye.renderOy     += eyeHop;
        mouth.renderOy        += bounce * 1.1f;
        mouth.renderOpenBoost  = (bounce * 0.5f + 0.5f) * 0.35f;
        break;
      }
      case EMO_CRY: {
        const float quiver = sinf(t * 24.0f) * 0.65f;
        mouth.renderOy    += quiver;
        leftEye.renderOy  += sinf(t * 6.0f) * 0.5f;
        rightEye.renderOy += sinf(t * 6.0f) * 0.5f;
        break;
      }
      case EMO_CONFUSED: {
        const float swayX = sinf(t * 2.1f) * 2.2f;
        const float swayY = cosf(t * 2.1f) * 0.8f;
        leftEye.renderOx  += swayX;
        rightEye.renderOx += swayX;
        leftEye.renderOy  += swayY;
        rightEye.renderOy -= swayY;
        break;
      }
      case EMO_DIZZY: {
        const float sx = sinf(t * 3.4f) * 4.2f;
        const float sy = sinf(t * 6.8f) * 1.8f;
        leftEye.renderOx  += sx;
        rightEye.renderOx += sx;
        leftEye.renderOy  += sy;
        rightEye.renderOy -= sy;
        mouth.renderOx    += sx * 0.55f;
        break;
      }
      case EMO_SUSPICIOUS: {
        gazeX_.set(sinf(t * 1.35f) * 9.5f);
        gazeY_.set(-1.0f);
        break;
      }
      case EMO_SCARED: {
        const float sx = sinf(t * 58.0f) * 1.25f;
        const float sy = cosf(t * 63.0f) * 0.90f;
        leftEye.renderOx  += sx;
        rightEye.renderOx += sx;
        leftEye.renderOy  += sy;
        rightEye.renderOy += sy;
        mouth.renderOx    += sx * 0.85f;
        mouth.renderOy    += sy * 0.65f;
        break;
      }
      default:
        break;
    }
  }

  // --- Particle Simulation ---
  void updateParticles_(float dt) {
    fx.fxTime += dt;

    if (fx.showSweat) {
      fx.sweatPhase = fmodf(fx.sweatPhase + dt * 0.68f, 1.0f);
    }

    if (fx.showTears) {
      for (int i = 0; i < 2; i++) {
        TearDrop &td = fx.tears[i];
        if (!td.active) {
          td.respawnDelay -= dt;
          if (td.respawnDelay <= 0.0f) {
            const AnimatedEye &eye = (i == 0) ? leftEye : rightEye;
            td.x      = eye.x.val + eye.renderOx + (i == 0 ? -6.0f : 6.0f);
            td.y      = eye.y.val + eye.renderOy + eye.h.val * 0.45f;
            td.vy     = 14.0f;
            td.active = true;
          }
        } else {
          td.vy += 68.0f * dt;  // gentle gravity acceleration
          td.y  += td.vy * dt;
          if (td.y > SCREEN_HEIGHT + 4.0f) {
            td.active       = false;
            td.respawnDelay = static_cast<float>(random(150, 480)) * 0.001f;
          }
        }
      }
    }

    if (fx.floaterMode != FLOATER_NONE) {
      for (int i = 0; i < 3; i++) {
        FloatParticle &p = fx.floaters[i];
        p.phase += p.speed * dt;
        if (p.phase >= 1.0f) {
          p.phase -= 1.0f;
        }

        if (fx.floaterMode == FLOATER_ZZZ) {
          p.x = 104.0f + p.phase * 14.0f + sinf(p.phase * 7.0f + i) * 2.5f;
          p.y = 28.0f - p.phase * 24.0f;
        } else if (fx.floaterMode == FLOATER_HEARTS) {
          const float baseX = (i == 0) ? 11.0f : ((i == 1) ? 116.0f : 64.0f);
          const float sway  = sinf(p.phase * 6.283f + i * 2.0f) * 3.0f;
          p.x = baseX + sway;
          p.y = (i == 2) ? (12.0f - p.phase * 9.0f) : (38.0f - p.phase * 32.0f);
        } else if (fx.floaterMode == FLOATER_SPARKLES) {
          static constexpr float sx[3] = {12.0f, 115.0f, 64.0f};
          static constexpr float sy[3] = {11.0f, 13.0f, 6.0f};
          p.x = sx[i];
          p.y = sy[i];
        }
      }
    }
  }

  // --- Serial Command Parser ---
  void processCommand_(char *raw) {
    // Trim leading spaces
    while (*raw == ' ' || *raw == '\t') raw++;
    // Trim trailing spaces
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t')) {
      raw[--len] = '\0';
    }
    if (len == 0) return;

    // Lowercase in-place
    for (size_t i = 0; i < len; i++) {
      if (raw[i] >= 'A' && raw[i] <= 'Z') {
        raw[i] = static_cast<char>(raw[i] - 'A' + 'a');
      }
    }

    if (strcmp(raw, "help") == 0 || strcmp(raw, "?") == 0) {
      printHelp();
      return;
    }
    if (strcmp(raw, "auto") == 0) {
      music.active   = false;
      autoMode       = true;
      nextEmotionIn_ = 2.0f;
      Serial.println(F("[MODE] Autonomous ON"));
      return;
    }
    if (strcmp(raw, "music off") == 0 || strcmp(raw, "m,off") == 0) {
      music.active   = false;
      autoMode       = true;
      nextEmotionIn_ = 1.8f;
      applyEmotionTargets_(EMO_NEUTRAL, true);
      Serial.println(F("[MUSIC] Sync OFF -> Autonomous ON"));
      return;
    }
    if (strncmp(raw, "m,", 2) == 0) {
      handleMusicPacket_(raw + 2);
      return;
    }
    if (strcmp(raw, "next") == 0) {
      music.active = false;
      autoMode     = false;
      Emotion next = static_cast<Emotion>((static_cast<uint8_t>(currentEmotion) + 1) % EMOTION_COUNT);
      setEmotion(next);
      Serial.print(F("[EMO] Locked: "));
      Serial.println(EMOTION_NAMES[next]);
      return;
    }
    if (strcmp(raw, "blink") == 0) {
      triggerBlink(false, false);
      Serial.println(F("[ACT] Blink"));
      return;
    }
    if (strcmp(raw, "dblink") == 0) {
      triggerBlink(true, false);
      Serial.println(F("[ACT] Double Blink"));
      return;
    }
    if (strcmp(raw, "qwink") == 0) {
      triggerWink();
      Serial.println(F("[ACT] Quick Wink"));
      return;
    }
    if (strcmp(raw, "look") == 0) {
      triggerSaccade();
      Serial.println(F("[ACT] Saccade"));
      return;
    }
    if (strcmp(raw, "boot") == 0) {
      startBootSequence();
      Serial.println(F("[ACT] Boot Wake-Up Sequence"));
      return;
    }
    if (strcmp(raw, "fps") == 0) {
      fpsReportEnabled = !fpsReportEnabled;
      Serial.print(F("[FPS] Telemetry "));
      Serial.println(fpsReportEnabled ? F("ON") : F("OFF"));
      return;
    }
    if (strcmp(raw, "refresh") == 0) {
      cmdRequestRefresh = true;
      Serial.println(F("[OLED] Full resync requested"));
      return;
    }
    if (strcmp(raw, "partial on") == 0) {
      cmdTogglePartial = true;
      cmdPartialValue  = true;
      Serial.println(F("[OLED] Dirty-Tile Partial Flush: ON"));
      return;
    }
    if (strcmp(raw, "partial off") == 0) {
      cmdTogglePartial = true;
      cmdPartialValue  = false;
      Serial.println(F("[OLED] Dirty-Tile Partial Flush: OFF (Full-Frame)"));
      return;
    }
    if (strncmp(raw, "i2c ", 4) == 0) {
      long khz = atol(raw + 4);
      if (khz >= 100 && khz <= 1000) {
        cmdChangeI2C = true;
        cmdNewI2CHz  = static_cast<uint32_t>(khz) * 1000UL;
        Serial.print(F("[I2C] Clock set to "));
        Serial.print(khz);
        Serial.println(F(" kHz"));
      } else {
        Serial.println(F("[ERR] Use i2c 100..1000 (recommended: 400)"));
      }
      return;
    }

    // Match emotion by name
    for (uint8_t i = 0; i < EMOTION_COUNT; i++) {
      if (strcmp(raw, EMOTION_NAMES[i]) == 0) {
        music.active = false;
        autoMode     = false;
        setEmotion(static_cast<Emotion>(i));
        Serial.print(F("[EMO] Locked: "));
        Serial.println(EMOTION_NAMES[i]);
        return;
      }
    }

    // Match emotion by numeric index 0..14
    bool allDigits = true;
    for (size_t i = 0; i < len; i++) {
      if (raw[i] < '0' || raw[i] > '9') {
        allDigits = false;
        break;
      }
    }
    if (allDigits) {
      int idx = atoi(raw);
      if (idx >= 0 && idx < EMOTION_COUNT) {
        music.active = false;
        autoMode     = false;
        setEmotion(static_cast<Emotion>(idx));
        Serial.print(F("[EMO] Locked: "));
        Serial.println(EMOTION_NAMES[idx]);
        return;
      }
    }

    Serial.print(F("[ERR] Unknown command: "));
    Serial.print(raw);
    Serial.println(F(" (ketik 'help')"));
  }

  // Parses compact PC-bridge music packet: "<emo>,<energy>,<bass>,<vocal>,<beat>"
  void handleMusicPacket_(char *payload) {
    int vals[5] = {0, 0, 0, 0, 0};
    int count   = 0;
    char *tok   = payload;

    while (tok != nullptr && count < 5) {
      char *comma = strchr(tok, ',');
      if (comma != nullptr) {
        *comma = '\0';
      }
      vals[count++] = atoi(tok);
      tok = (comma != nullptr) ? (comma + 1) : nullptr;
    }

    if (count < 4) return;

    const int emoIdx    = clampi(vals[0], 0, static_cast<int>(EMOTION_COUNT) - 1);
    const float energy  = clampf(static_cast<float>(vals[1]) * 0.01f, 0.0f, 1.0f);
    const float bass    = clampf(static_cast<float>(vals[2]) * 0.01f, 0.0f, 1.0f);
    const float vocal   = clampf(static_cast<float>(vals[3]) * 0.01f, 0.0f, 1.0f);
    const bool beatHit  = (count >= 5) && (vals[4] > 0);

    bootActive_         = false;
    autoMode            = false;
    music.active        = true;
    music.energy        = energy;
    music.bass          = bass;
    music.vocal         = vocal;
    music.sincePacketS  = 0.0f;

    const Emotion targetEmo = static_cast<Emotion>(emoIdx);
    if (targetEmo != currentEmotion) {
      applyEmotionTargets_(targetEmo, false);
    }

    if (beatHit) {
      music.beatPulse = 1.0f;
      if (leftEye.canBlink && rightEye.canBlink && blinkState_ == BLINK_IDLE) {
        leftEye.y.impulse(18.0f + bass * 22.0f);
        rightEye.y.impulse(18.0f + bass * 22.0f);
      }
    }
  }
};
