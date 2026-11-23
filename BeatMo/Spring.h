#pragma once

#include <math.h>
#include "Config.h"

// ============================================================================
// Math & Delta-Time 2nd-Order Spring-Damper Physics
// ============================================================================
inline float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

inline float lerpf(float a, float b, float t) {
  return a + (b - a) * t;
}

inline int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

struct SpringVal {
  float val    = 0.0f;
  float vel    = 0.0f;
  float target = 0.0f;
  float omega  = 16.0f;
  float zeta   = 0.75f;

  void configure(float omegaRad, float dampingRatio) {
    omega = omegaRad;
    zeta  = dampingRatio;
  }

  void set(float t) {
    target = t;
  }

  void snap(float v) {
    val    = v;
    target = v;
    vel    = 0.0f;
  }

  void impulse(float dv) {
    vel += dv;
  }

  bool isSettled() const {
    return (fabsf(target - val) < 0.04f) && (fabsf(vel) < 0.08f);
  }

  void update(float dt) {
    if (dt <= 0.0f) return;
    if (dt > MAX_DT_SEC) dt = MAX_DT_SEC;

    // Sub-step when dt > 12ms so semi-implicit Euler stays unconditionally stable
    const int steps = (dt > 0.012f) ? 2 : 1;
    const float h   = dt / static_cast<float>(steps);
    const float k   = omega * omega;
    const float c   = 2.0f * zeta * omega;

    for (int i = 0; i < steps; i++) {
      const float acc = k * (target - val) - c * vel;
      vel += acc * h;
      val += vel * h;
    }

    // Deadzone snap eliminates endless 1-pixel tail jitter
    if (fabsf(target - val) < 0.04f && fabsf(vel) < 0.08f) {
      val = target;
      vel = 0.0f;
    }
  }
};
