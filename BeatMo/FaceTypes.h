#pragma once

#include <Arduino.h>
#include "Config.h"
#include "Spring.h"

// ============================================================================
// Emotions & Visual Styles
// ============================================================================
enum Emotion : uint8_t {
  EMO_NEUTRAL = 0,
  EMO_HAPPY,
  EMO_SAD,
  EMO_SURPRISED,
  EMO_SLEEPY,
  EMO_ANGRY,
  EMO_LOVE,
  EMO_LAUGH,
  EMO_CRY,
  EMO_WINK,
  EMO_CONFUSED,
  EMO_SHY,
  EMO_DIZZY,
  EMO_SUSPICIOUS,
  EMO_SCARED,
  EMOTION_COUNT
};

static const char* const EMOTION_NAMES[EMOTION_COUNT] = {
  "neutral",
  "happy",
  "sad",
  "surprised",
  "sleepy",
  "angry",
  "love",
  "laugh",
  "cry",
  "wink",
  "confused",
  "shy",
  "dizzy",
  "suspicious",
  "scared"
};

enum EyeStyle : uint8_t {
  EYE_NORMAL = 0,
  EYE_HEART,
  EYE_DIZZY,
  EYE_SCARED_PUPIL
};

enum MouthStyle : uint8_t {
  MOUTH_CURVE = 0,
  MOUTH_OPEN_O,
  MOUTH_CAT,
  MOUTH_WAVY
};

enum FloaterMode : uint8_t {
  FLOATER_NONE = 0,
  FLOATER_ZZZ,
  FLOATER_HEARTS,
  FLOATER_SPARKLES
};

// ============================================================================
// Animated Eye State
// ============================================================================
struct AnimatedEye {
  SpringVal x, y, w, h;
  SpringVal roundness;
  SpringVal topInner;   // 0..1 top lid drop on inner (nose) side
  SpringVal topOuter;   // 0..1 top lid drop on outer side
  SpringVal bottomLid;  // 0..1 bottom smiling cheek push upward

  EyeStyle style = EYE_NORMAL;
  bool isLeft    = true;
  bool canBlink  = true;

  // Per-frame transient modifiers (gaze, breath, shake, bounce, blink)
  float renderOx     = 0.0f;
  float renderOy     = 0.0f;
  float renderScaleW = 1.0f;
  float renderScaleH = 1.0f;
  float blinkClose   = 0.0f;  // 0 = open, 1 = fully shut slit
  float stylePhase   = 0.0f;  // spiral spin / scared pupil tremor

  void configureSprings() {
    x.configure(SPRING_POS_OMEGA, SPRING_POS_ZETA);
    y.configure(SPRING_POS_OMEGA, SPRING_POS_ZETA);
    w.configure(SPRING_SIZE_OMEGA, SPRING_SIZE_ZETA);
    h.configure(SPRING_SIZE_OMEGA, SPRING_SIZE_ZETA);
    roundness.configure(SPRING_SHAPE_OMEGA, SPRING_SHAPE_ZETA);
    topInner.configure(SPRING_LID_OMEGA, SPRING_LID_ZETA);
    topOuter.configure(SPRING_LID_OMEGA, SPRING_LID_ZETA);
    bottomLid.configure(SPRING_LID_OMEGA, SPRING_LID_ZETA);
  }

  void snapAll() {
    x.snap(x.target);
    y.snap(y.target);
    w.snap(w.target);
    h.snap(h.target);
    roundness.snap(roundness.target);
    topInner.snap(topInner.target);
    topOuter.snap(topOuter.target);
    bottomLid.snap(bottomLid.target);
  }

  void update(float dt) {
    x.update(dt);
    y.update(dt);
    w.update(dt);
    h.update(dt);
    roundness.update(dt);
    topInner.update(dt);
    topOuter.update(dt);
    bottomLid.update(dt);
  }
};

// ============================================================================
// Animated Mouth State
// ============================================================================
struct AnimatedMouth {
  SpringVal x, y, w, h;
  SpringVal curve;  // -1.0 (frown) .. 0.0 (flat) .. +1.0 (smile)
  SpringVal open;   //  0.0 (closed) .. 1.0 (wide open grin / O)

  MouthStyle style      = MOUTH_CURVE;
  float renderOx        = 0.0f;
  float renderOy        = 0.0f;
  float renderOpenBoost = 0.0f;
  float wavePhase       = 0.0f;

  void configureSprings() {
    x.configure(SPRING_MOUTH_OMEGA, SPRING_MOUTH_ZETA);
    y.configure(SPRING_MOUTH_OMEGA, SPRING_MOUTH_ZETA);
    w.configure(SPRING_MOUTH_OMEGA, SPRING_MOUTH_ZETA);
    h.configure(SPRING_MOUTH_OMEGA, SPRING_MOUTH_ZETA);
    curve.configure(SPRING_SHAPE_OMEGA, SPRING_SHAPE_ZETA);
    open.configure(SPRING_MOUTH_OMEGA, SPRING_MOUTH_ZETA);
  }

  void snapAll() {
    x.snap(x.target);
    y.snap(y.target);
    w.snap(w.target);
    h.snap(h.target);
    curve.snap(curve.target);
    open.snap(open.target);
  }

  void update(float dt) {
    x.update(dt);
    y.update(dt);
    w.update(dt);
    h.update(dt);
    curve.update(dt);
    open.update(dt);
  }
};

// ============================================================================
// Particle & Overlay FX State (Static Allocation — Zero Heap Usage)
// ============================================================================
struct TearDrop {
  float x    = 0.0f;
  float y    = 0.0f;
  float vy   = 0.0f;
  bool active = false;
  float respawnDelay = 0.0f;
};

struct FloatParticle {
  float x     = 0.0f;
  float y     = 0.0f;
  float phase = 0.0f;  // 0..1 normalized lifetime
  float speed = 0.5f;
  uint8_t slot = 0;
};

struct FaceFX {
  FloaterMode floaterMode = FLOATER_NONE;
  FloatParticle floaters[3];
  TearDrop tears[2];

  bool showBlush     = false;
  bool showAngerVein = false;
  bool showQuestion  = false;
  bool showSweat     = false;
  bool showTears     = false;

  float fxTime     = 0.0f;
  float sweatPhase = 0.0f;
};

// ============================================================================
// Music-Reactive State (Driven via USB Serial 'M,...' Packets)
// ============================================================================
struct MusicState {
  bool active        = false;
  float energy       = 0.0f;  // 0..1 overall track energy
  float bass         = 0.0f;  // 0..1 low-frequency energy
  float vocal        = 0.0f;  // 0..1 vocal/mid-band energy
  float smoothVocal  = 0.0f;  // smoothed lip-sync envelope
  float beatPulse    = 0.0f;  // 0..1 fast-decaying beat hit envelope
  float sincePacketS = 0.0f;  // watchdog timer (auto-exits if > 5s silent)
};
