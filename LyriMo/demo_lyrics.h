#pragma once

#include <Arduino.h>

struct DemoLyricLine {
  uint32_t timeMs;
  const char* text;
};

// Judul & artis sengaja dibuat cukup panjang agar efek marquee auto-scroll langsung terlihat
static const char* DEMO_HEADER = "Midnight Echoes - ESP32 Offline Music Box";
static const uint32_t DEMO_DURATION_MS = 48000UL;

// Daftar lirik demo offline: [timestamp_ms, "baris lirik"]
// String kosong ("") menampilkan ikon nada musik (jeda instrumental).
static const DemoLyricLine DEMO_LYRICS[] = {
    {0UL, ""},
    {3500UL, "Neon lights glow softly"},
    {7500UL, "Through the quiet midnight rain"},
    {12000UL, "Every pixel on this tiny screen carries a melody"},
    {17500UL, "Wait for the beat to drop"},
    {21500UL, ""},
    {25500UL, "Digital hearts still hum the same familiar chorus"},
    {31000UL, "Holding onto silver echoes until the morning comes around"},
    {37000UL, "Connect USB anytime to sync live with BitChord"},
    {42500UL, "Offline or live, the rhythm keeps moving on"},
    {46000UL, ""},
};

static const size_t DEMO_LYRIC_COUNT = sizeof(DEMO_LYRICS) / sizeof(DEMO_LYRICS[0]);
