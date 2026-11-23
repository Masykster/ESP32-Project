"""Music & Lyric Emotion Bridge for BeatMo (ESP32 + SSD1306).

Combines:
1. Windows SMTC + synced lyrics from LyriMo/pc_bridge/lyric_bridge.py (read-only import)
2. Real-time audio FFT analysis (WASAPI loopback via pyaudiowpatch, or Stereo Mix / Mic via sounddevice)
3. Hybrid 3-layer mood classification (active lyric sentiment + track title/artist + audio energy/beat)

Streams compact 25 Hz packets over USB Serial:
  M,<emo_idx>,<energy_0_100>,<bass_0_100>,<vocal_0_100>,<beat_0_1>
"""

from __future__ import annotations

import argparse
import asyncio
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
import importlib.util
import math
from pathlib import Path
import re
import sys
import threading
import time
from typing import Any

import numpy as np
import serial
from serial.tools import list_ports

try:
    from rich.console import Console
    from rich.live import Live
    from rich.panel import Panel
    from rich.table import Table
    HAS_RICH = True
except Exception:
    HAS_RICH = False


# ============================================================================
# Emotion Indices (must match enum Emotion in FaceTypes.h)
# ============================================================================
EMO_NEUTRAL    = 0
EMO_HAPPY      = 1
EMO_SAD        = 2
EMO_SURPRISED  = 3
EMO_SLEEPY     = 4
EMO_ANGRY      = 5
EMO_LOVE       = 6
EMO_LAUGH      = 7
EMO_CRY        = 8
EMO_WINK       = 9
EMO_CONFUSED   = 10
EMO_SHY        = 11
EMO_DIZZY      = 12
EMO_SUSPICIOUS = 13
EMO_SCARED     = 14

EMO_NAMES = (
    "neutral", "happy", "sad", "surprised", "sleepy",
    "angry", "love", "laugh", "cry", "wink",
    "confused", "shy", "dizzy", "suspicious", "scared",
)

DEFAULT_LYRIC_BRIDGE = (
    Path(__file__).resolve().parent.parent
    / "LyriMo"
    / "pc_bridge"
    / "lyric_bridge.py"
)


def load_lyric_bridge(path: Path) -> Any | None:
    """Dynamically import lyric_bridge.py without modifying the source project."""
    if not path.exists():
        return None
    try:
        spec = importlib.util.spec_from_file_location("external_lyric_bridge", str(path))
        if spec is None or spec.loader is None:
            return None
        mod = importlib.util.module_from_spec(spec)
        # Register in sys.modules before exec_module so @dataclass works on Python 3.13
        sys.modules["external_lyric_bridge"] = mod
        spec.loader.exec_module(mod)
        return mod
    except Exception as exc:
        print(f"[WARN] Failed to load lyric_bridge.py ({exc}); running SMTC/Audio fallback.")
        return None


# ============================================================================
# Bilingual (EN + ID) Lyric & Title Sentiment Lexicon
# ============================================================================
LYRIC_KEYWORDS: dict[int, tuple[tuple[str, float], ...]] = {
    EMO_CRY: (
        ("cry", 2.2), ("crying", 2.4), ("cried", 2.2), ("tears", 2.3), ("tear", 1.8),
        ("weep", 2.2), ("sob", 2.2), ("heartbreak", 2.5), ("heartbroken", 2.5),
        ("dying inside", 2.5), ("bleeding", 2.0), ("goodbye", 1.6),
        ("menangis", 2.5), ("tangis", 2.4), ("nangis", 2.5), ("air mata", 2.7),
        ("hancur", 2.2), ("perih", 2.2), ("tersakiti", 2.3), ("kehilangan", 2.1),
        ("pedih", 2.3), ("pilu", 2.4), ("remuk", 2.3), ("menyesak", 2.2),
    ),
    EMO_SAD: (
        ("sad", 2.0), ("lonely", 2.1), ("alone", 1.7), ("broken", 2.0), ("hurt", 1.9),
        ("pain", 1.8), ("miss you", 2.2), ("missing you", 2.2), ("lost", 1.5),
        ("empty", 1.8), ("rain", 1.4), ("sorry", 1.7), ("regret", 2.0), ("blue", 1.3),
        ("gone", 1.5), ("leave", 1.5), ("fade", 1.4), ("cold", 1.3), ("without you", 2.1),
        ("sedih", 2.2), ("sepi", 2.0), ("sendiri", 1.8), ("galau", 2.3),
        ("kecewa", 2.1), ("luka", 2.2), ("rapuh", 2.1), ("pergi", 1.6),
        ("menyesal", 2.1), ("sesal", 2.1), ("sunyi", 1.9), ("tanpa", 1.6),
        ("hilang", 1.7), ("maaf", 2.1), ("tinggal", 1.6), ("layu", 1.9),
        ("usang", 1.8), ("ego", 1.7), ("hampa", 2.1), ("sesak", 2.0),
        ("berakhir", 1.8), ("jauh", 1.4), ("tak bisa", 1.6), ("sakit", 1.9),
    ),
    EMO_LOVE: (
        ("love", 2.0), ("loving", 2.1), ("in love", 2.6), ("heart", 1.7), ("kiss", 2.1),
        ("darling", 2.2), ("honey", 1.9), ("sweetheart", 2.3), ("forever", 1.7),
        ("hold me", 1.9), ("hug", 1.8), ("romance", 2.2), ("angel", 1.8), ("belong", 1.6),
        ("baby", 1.2), ("you and me", 1.9), ("beautiful", 1.6), ("only you", 2.0),
        ("cinta", 2.3), ("sayang", 2.3), ("kasih", 2.0), ("rindu", 2.4),
        ("peluk", 2.1), ("kekasih", 2.2), ("hati", 1.5), ("asmara", 2.3),
        ("mesra", 2.2), ("jatuh cinta", 2.6), ("selamanya", 1.6), ("untukmu", 1.8),
        ("bersama", 1.5), ("bunga", 1.3), ("setia", 1.8), ("jiwa", 1.4),
    ),
    EMO_SHY: (
        ("shy", 2.4), ("blush", 2.5), ("blushing", 2.5), ("nervous", 1.9),
        ("butterflies", 2.2), ("crush", 2.1), ("staring at you", 2.0), ("secret", 1.6),
        ("malu", 2.4), ("salting", 2.6), ("deg-degan", 2.5), ("grogi", 2.2),
        ("tersipu", 2.5), ("merona", 2.5), ("diam-diam", 1.7), ("berdebar", 2.2),
    ),
    EMO_LAUGH: (
        ("laugh", 2.3), ("laughing", 2.4), ("haha", 2.5), ("hahaha", 2.6),
        ("smile so wide", 2.2), ("party", 2.0), ("celebrate", 2.1), ("woo", 1.8),
        ("yeah yeah", 1.6), ("jump", 1.7), ("go crazy", 1.8),
        ("tertawa", 2.4), ("ketawa", 2.4), ("ngakak", 2.6), ("pesta", 2.1),
        ("meriah", 2.0), ("sorak", 2.1),
    ),
    EMO_HAPPY: (
        ("happy", 2.2), ("joy", 2.2), ("smile", 1.9), ("sunshine", 2.0), ("sunny", 1.9),
        ("bright", 1.6), ("dance", 1.9), ("dancing", 2.0), ("good time", 2.1),
        ("feel good", 2.2), ("alive", 1.8), ("paradise", 1.9), ("heaven", 1.6),
        ("bahagia", 2.3), ("senang", 2.2), ("gembira", 2.3), ("senyum", 2.0),
        ("ceria", 2.2), ("menari", 2.0), ("bersinar", 1.7), ("indah", 1.7),
        ("hangat", 1.6), ("cerah", 1.8), ("syukur", 1.9),
    ),
    EMO_ANGRY: (
        ("angry", 2.4), ("rage", 2.5), ("hate", 2.3), ("mad", 2.0), ("fury", 2.5),
        ("scream", 2.1), ("fight", 2.0), ("kill", 2.2), ("burn", 1.9), ("hell", 1.9),
        ("damn", 1.8), ("furious", 2.5), ("destroy", 2.2), ("revenge", 2.3),
        ("marah", 2.5), ("benci", 2.5), ("murka", 2.6), ("muak", 2.2),
        ("hancurkan", 2.3), ("teriak", 2.0), ("dendam", 2.3), ("kesal", 2.0),
        ("keras hati", 2.0), ("amarah", 2.5),
    ),
    EMO_SCARED: (
        ("scared", 2.4), ("afraid", 2.3), ("fear", 2.2), ("terrified", 2.6),
        ("monster", 2.1), ("ghost", 2.1), ("shadow", 1.7), ("nightmare", 2.4),
        ("panic", 2.3), ("run away", 1.8), ("haunted", 2.3), ("tremble", 2.1),
        ("takut", 2.4), ("hantu", 2.2), ("mimpi buruk", 2.5), ("gelap", 1.6),
        ("gemetar", 2.2), ("panik", 2.3), ("ngeri", 2.4), ("bayang", 1.5),
    ),
    EMO_SURPRISED: (
        ("surprise", 2.2), ("whoa", 2.0), ("oh my god", 2.4), ("omg", 2.3),
        ("suddenly", 2.0), ("shock", 2.3), ("unbelievable", 2.2), ("wow", 2.1),
        ("kaget", 2.4), ("kejut", 2.3), ("astaga", 2.2), ("tiba-tiba", 2.0),
        ("ternyata", 1.8),
    ),
    EMO_CONFUSED: (
        ("confused", 2.4), ("wonder why", 2.2), ("why", 1.4), ("don't know", 1.9),
        ("dont know", 1.9), ("what is", 1.4), ("puzzle", 2.0), ("maze", 1.9),
        ("bingung", 2.4), ("kenapa", 1.8), ("mengapa", 1.8), ("tak tahu", 2.0),
        ("entahlah", 2.2), ("aneh", 1.6), ("ragu", 1.7), ("mungkinkah", 1.8),
        ("akankah", 1.6), ("masihkah", 1.6), ("bagaimana", 1.6),
    ),
    EMO_SLEEPY: (
        ("sleep", 2.3), ("sleepy", 2.5), ("dream", 1.7), ("dreaming", 1.8),
        ("lullaby", 2.5), ("tired", 2.1), ("midnight", 1.7), ("rest", 1.8),
        ("goodnight", 2.4), ("close my eyes", 2.2), ("slowly", 1.5),
        ("tidur", 2.4), ("mengantuk", 2.5), ("ngantuk", 2.5), ("mimpi", 1.7),
        ("lelah", 2.0), ("penat", 2.1), ("malam", 1.4), ("tenang", 1.5),
        ("angin", 1.2), ("pejam", 2.1),
    ),
    EMO_DIZZY: (
        ("dizzy", 2.5), ("spinning", 2.4), ("spin", 2.1), ("round and round", 2.4),
        ("drunk", 2.2), ("crazy", 1.8), ("psycho", 2.0), ("blurry", 2.1),
        ("pusing", 2.5), ("berputar", 2.3), ("mabuk", 2.3), ("gila", 1.8),
        ("oleng", 2.4),
    ),
    EMO_SUSPICIOUS: (
        ("lie", 2.0), ("liar", 2.4), ("lying", 2.2), ("fake", 2.1), ("cheat", 2.3),
        ("suspect", 2.3), ("betray", 2.3), ("trust no one", 2.4),
        ("bohong", 2.4), ("palsu", 2.2), ("curiga", 2.5), ("khianat", 2.4),
        ("munafik", 2.5), ("selingkuh", 2.4), ("dusta", 2.4),
    ),
    EMO_WINK: (
        ("wink", 2.5), ("flirt", 2.4), ("tease", 2.2), ("cheeky", 2.3),
        ("cool", 1.4), ("smooth", 1.6), ("playboy", 2.0),
        ("goda", 2.3), ("menggoda", 2.4), ("nakal", 2.0), ("genit", 2.4),
    ),
}

TITLE_GENRE_HINTS: tuple[tuple[str, int, float], ...] = (
    ("lofi", EMO_SLEEPY, 1.8),
    ("lo-fi", EMO_SLEEPY, 1.8),
    ("chill", EMO_SLEEPY, 1.5),
    ("sleep", EMO_SLEEPY, 2.0),
    ("ambient", EMO_SLEEPY, 1.7),
    ("acoustic", EMO_SAD, 1.2),
    ("sad", EMO_SAD, 1.9),
    ("galau", EMO_SAD, 2.2),
    ("ballad", EMO_SAD, 1.4),
    ("maaf", EMO_SAD, 1.9),
    ("luka", EMO_SAD, 1.9),
    ("love", EMO_LOVE, 1.8),
    ("romantic", EMO_LOVE, 2.0),
    ("cinta", EMO_LOVE, 2.0),
    ("rindu", EMO_LOVE, 1.9),
    ("phonk", EMO_ANGRY, 2.0),
    ("metal", EMO_ANGRY, 2.0),
    ("rock", EMO_ANGRY, 1.2),
    ("remix", EMO_LAUGH, 1.2),
    ("party", EMO_LAUGH, 1.8),
    ("dance", EMO_HAPPY, 1.6),
    ("funk", EMO_HAPPY, 1.7),
    ("disco", EMO_HAPPY, 1.7),
    ("nightcore", EMO_SURPRISED, 1.3),
)

_ID_SUFFIX_STEM_RE = r"(?:ku|mu|nya|kan|lah|pun|i)?"


def score_text_keywords(text: str, extra_hints: bool = False) -> dict[int, float]:
    """Compute per-emotion affinity scores from a lyric line or track title."""
    scores: dict[int, float] = {}
    if not text:
        return scores

    raw_stripped = text.strip()
    low = " " + re.sub(r"[^a-z0-9\s\-']", " ", text.lower()) + " "
    for emo, pairs in LYRIC_KEYWORDS.items():
        total = 0.0
        for kw, weight in pairs:
            if " " in kw:
                if f" {kw}" in low:
                    total += weight
            else:
                # Allow common Indonesian possessive/verb suffixes (e.g. maafku, hadirmu, tinggalkan)
                pattern = rf"\b{re.escape(kw)}{_ID_SUFFIX_STEM_RE}\b"
                if re.search(pattern, low):
                    total += weight
        if total > 0.0:
            scores[emo] = total

    # Question mark gives a subtle curious/confused nudge
    if "?" in raw_stripped:
        scores[EMO_CONFUSED] = scores.get(EMO_CONFUSED, 0.0) + 1.15

    # Exclamation mark amplifies high-arousal feelings if already present
    if "!" in raw_stripped:
        for hi_emo in (EMO_HAPPY, EMO_LAUGH, EMO_ANGRY, EMO_SURPRISED, EMO_CRY):
            if hi_emo in scores:
                scores[hi_emo] *= 1.25

    if extra_hints:
        for kw, emo, weight in TITLE_GENRE_HINTS:
            if re.search(rf"\b{re.escape(kw)}{_ID_SUFFIX_STEM_RE}\b", low):
                scores[emo] = scores.get(emo, 0.0) + weight

    return scores


# ============================================================================
# Real-Time Audio Capture & FFT DSP
# ============================================================================
@dataclass
class AudioSnapshot:
    energy: float = 0.0     # 0..1
    bass: float = 0.0       # 0..1
    vocal: float = 0.0      # 0..1
    treble: float = 0.0     # 0..1
    beat: bool = False
    beat_rate_hz: float = 0.0
    source_name: str = "NONE"


class AudioAnalyzer:
    """Thread-safe audio loopback FFT analyzer (pyaudiowpatch -> sounddevice fallback)."""

    def __init__(self, device_index: int | None = None, disabled: bool = False) -> None:
        self.disabled = disabled
        self.device_index = device_index
        self.source_name = "OFF (Lyric-Pulse)" if disabled else "Initializing..."

        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

        self._samplerate = 48000
        self._fft_size = 1024
        self._hann = np.hanning(self._fft_size).astype(np.float32)
        self._ring = np.zeros(self._fft_size, dtype=np.float32)

        # Adaptive normalization & beat detector state
        self._rms_peak = 0.03
        self._bass_peak = 0.03
        self._vocal_peak = 0.03
        self._treble_peak = 0.02
        self._bass_avg = 0.005
        self._last_beat_t = 0.0
        self._beat_times: deque[float] = deque(maxlen=16)

        self._energy = 0.0
        self._bass = 0.0
        self._vocal = 0.0
        self._treble = 0.0
        self._pending_beat = False
        self._beat_rate_hz = 0.0

    def start(self) -> None:
        if self.disabled:
            return
        self._thread = threading.Thread(target=self._run_capture, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.5)

    def read_snapshot(self) -> AudioSnapshot:
        with self._lock:
            beat = self._pending_beat
            self._pending_beat = False
            return AudioSnapshot(
                energy=self._energy,
                bass=self._bass,
                vocal=self._vocal,
                treble=self._treble,
                beat=beat,
                beat_rate_hz=self._beat_rate_hz,
                source_name=self.source_name,
            )

    def _process_samples(self, mono: np.ndarray, sr: int) -> None:
        if mono.size == 0:
            return
        if sr != self._samplerate:
            self._samplerate = sr

        n = min(mono.size, self._fft_size)
        self._ring = np.roll(self._ring, -n)
        self._ring[-n:] = mono[-n:]

        rms = float(np.sqrt(np.mean(self._ring * self._ring)))
        if rms < 1e-6:
            rms = 0.0

        windowed = self._ring * self._hann
        spec = np.abs(np.fft.rfft(windowed))
        freqs = np.fft.rfftfreq(self._fft_size, d=1.0 / float(self._samplerate))

        bass_mask = (freqs >= 20.0) & (freqs <= 160.0)
        vocal_mask = (freqs >= 300.0) & (freqs <= 3000.0)
        treble_mask = (freqs >= 3000.0) & (freqs <= 10000.0)

        bass_raw = float(np.sqrt(np.mean(spec[bass_mask] ** 2))) if np.any(bass_mask) else 0.0
        vocal_raw = float(np.sqrt(np.mean(spec[vocal_mask] ** 2))) if np.any(vocal_mask) else 0.0
        treble_raw = float(np.sqrt(np.mean(spec[treble_mask] ** 2))) if np.any(treble_mask) else 0.0

        # Auto-gain with slow release so quiet & loud songs both stay expressive
        self._rms_peak = max(0.015, max(rms, self._rms_peak * 0.996))
        self._bass_peak = max(0.8, max(bass_raw, self._bass_peak * 0.996))
        self._vocal_peak = max(0.6, max(vocal_raw, self._vocal_peak * 0.996))
        self._treble_peak = max(0.4, max(treble_raw, self._treble_peak * 0.996))

        energy_norm = min(1.0, (rms / self._rms_peak) * 1.15)
        bass_norm = min(1.0, (bass_raw / self._bass_peak) * 1.2)
        vocal_norm = min(1.0, (vocal_raw / self._vocal_peak) * 1.25)
        treble_norm = min(1.0, (treble_raw / self._treble_peak) * 1.2)

        # Silence gate
        if rms < 0.0015:
            energy_norm = bass_norm = vocal_norm = treble_norm = 0.0

        now = time.monotonic()
        self._bass_avg = self._bass_avg * 0.90 + bass_raw * 0.10
        beat_hit = False
        if (
            bass_raw > max(1.2, self._bass_avg * 1.48)
            and bass_norm > 0.25
            and (now - self._last_beat_t) >= 0.19
        ):
            beat_hit = True
            self._last_beat_t = now
            self._beat_times.append(now)

        while self._beat_times and (now - self._beat_times[0]) > 4.0:
            self._beat_times.popleft()
        beat_rate = (len(self._beat_times) / 4.0) if len(self._beat_times) >= 2 else 0.0

        with self._lock:
            self._energy = self._energy * 0.45 + energy_norm * 0.55
            self._bass = self._bass * 0.40 + bass_norm * 0.60
            self._vocal = self._vocal * 0.35 + vocal_norm * 0.65
            self._treble = self._treble * 0.45 + treble_norm * 0.55
            if beat_hit:
                self._pending_beat = True
            self._beat_rate_hz = beat_rate

    def _run_capture(self) -> None:
        # Priority 1: PyAudioWPatch (true Windows WASAPI Loopback on default output)
        if self._try_pyaudiowpatch():
            return
        # Priority 2: sounddevice (Stereo Mix / Microphone fallback)
        if self._try_sounddevice():
            return
        self.source_name = "UNAVAILABLE (Lyric-Pulse Fallback)"

    def _try_pyaudiowpatch(self) -> bool:
        try:
            import pyaudiowpatch as pyaudio  # type: ignore
        except Exception:
            return False

        p = None
        stream = None
        try:
            p = pyaudio.PyAudio()
            wasapi_info = p.get_host_api_info_by_type(pyaudio.paWASAPI)
            default_out = p.get_device_info_by_index(wasapi_info["defaultOutputDevice"])

            target = None
            loopbacks = list(p.get_loopback_device_info_generator())
            if default_out.get("isLoopbackDevice", False):
                target = default_out
            else:
                out_name = default_out.get("name", "")
                for loop_dev in loopbacks:
                    if out_name and (out_name in loop_dev["name"] or loop_dev["name"].startswith(out_name[:18])):
                        target = loop_dev
                        break
                if target is None and loopbacks:
                    target = loopbacks[0]
            if target is None:
                return False

            sr = int(target["defaultSampleRate"])
            ch = max(1, int(target["maxInputChannels"]))
            self.source_name = f"WASAPI Loopback: {target['name']}"

            stream = p.open(
                format=pyaudio.paFloat32,
                channels=ch,
                rate=sr,
                input=True,
                input_device_index=int(target["index"]),
                frames_per_buffer=512,
            )
            while not self._stop.is_set():
                raw = stream.read(512, exception_on_overflow=False)
                arr = np.frombuffer(raw, dtype=np.float32)
                if ch > 1:
                    arr = arr.reshape(-1, ch).mean(axis=1)
                self._process_samples(arr, sr)
            return True
        except Exception as exc:
            self.source_name = f"WASAPI err ({exc})"
            return False
        finally:
            if stream is not None:
                try:
                    stream.stop_stream()
                    stream.close()
                except Exception:
                    pass
            if p is not None:
                try:
                    p.terminate()
                except Exception:
                    pass

    def _try_sounddevice(self) -> bool:
        try:
            import sounddevice as sd
        except Exception:
            return False

        try:
            devices = sd.query_devices()
            hostapis = sd.query_hostapis()
        except Exception as exc:
            self.source_name = f"sounddevice err ({exc})"
            return False

        candidates: list[int] = []
        if self.device_index is not None:
            candidates.append(self.device_index)
        else:
            # 1. Non-WDM-KS Stereo Mix first (WDM-KS frequently throws -9999 in PortAudio)
            for idx, d in enumerate(devices):
                hname = hostapis[d["hostapi"]]["name"]
                if d["max_input_channels"] > 0 and "stereo mix" in d["name"].lower() and "WDM-KS" not in hname:
                    candidates.append(idx)
            # 2. WASAPI / DirectSound / MME input devices (prefer default input)
            def_in = sd.default.device[0]
            if def_in is not None and def_in >= 0:
                candidates.append(int(def_in))
            for idx, d in enumerate(devices):
                hname = hostapis[d["hostapi"]]["name"]
                if d["max_input_channels"] > 0 and "WASAPI" in hname and idx not in candidates:
                    candidates.append(idx)
            for idx, d in enumerate(devices):
                hname = hostapis[d["hostapi"]]["name"]
                if d["max_input_channels"] > 0 and "WDM-KS" not in hname and idx not in candidates:
                    candidates.append(idx)

        last_err = ""
        for chosen_idx in candidates:
            if self._stop.is_set():
                return False
            try:
                dev_info = sd.query_devices(chosen_idx)
                sr = int(dev_info.get("default_samplerate") or 44100)
                ch = max(1, min(2, int(dev_info["max_input_channels"])))

                def _cb(indata: np.ndarray, frames: int, time_info: Any, status: Any) -> None:
                    if self._stop.is_set():
                        return
                    mono = indata.mean(axis=1) if indata.ndim > 1 else indata.reshape(-1)
                    self._process_samples(mono.astype(np.float32), sr)

                with sd.InputStream(
                    device=chosen_idx,
                    channels=ch,
                    samplerate=sr,
                    blocksize=512,
                    dtype="float32",
                    callback=_cb,
                ):
                    self.source_name = f"sounddevice[{chosen_idx}]: {dev_info['name']}"
                    while not self._stop.is_set():
                        time.sleep(0.05)
                return True
            except Exception as exc:
                last_err = str(exc)
                continue

        if last_err:
            self.source_name = f"sounddevice err ({last_err})"
        return False


# ============================================================================
# Hybrid 3-Layer Emotion Classifier with Song-Level Context & Momentum
# ============================================================================
class EmotionDirector:
    """Blends active lyric sentiment, lyric momentum, full-song mood, title hints & audio FFT."""

    def __init__(self) -> None:
        self.current_emo = EMO_NEUTRAL
        self.reason = "INIT"
        self._last_change_t = 0.0
        self._last_eval_t = 0.0
        self._last_track_key: tuple[str, str] | None = None
        self._title_priors: dict[int, float] = {}
        self._song_lyric_priors: dict[int, float] = {}
        self._lyric_momentum: dict[int, float] = {}
        self._active_lyric_scores: dict[int, float] = {}
        self._last_lyric_text = ""

    def on_track_change(self, title: str, artist: str) -> None:
        key = (title.strip(), artist.strip())
        if key == self._last_track_key:
            return
        self._last_track_key = key
        self._title_priors = score_text_keywords(f"{title} {artist}", extra_hints=True)
        self._song_lyric_priors = {}
        self._lyric_momentum = {}
        self._active_lyric_scores = {}
        self._last_lyric_text = ""

    def set_full_lyrics(self, entries: list[Any]) -> None:
        """Analyze the entire song's lyrics once fetched to build a baseline mood profile."""
        agg: dict[int, float] = {}
        for e in entries:
            txt = getattr(e, "text", "") or ""
            if not txt:
                continue
            sc = score_text_keywords(txt, extra_hints=False)
            for emo, w in sc.items():
                # Reduce single-line question noise in whole-song aggregation
                eff = w * 0.45 if emo == EMO_CONFUSED else w
                agg[emo] = agg.get(emo, 0.0) + eff
        if not agg:
            self._song_lyric_priors = {}
            return
        max_w = max(agg.values())
        self._song_lyric_priors = {
            emo: (w / max_w) * 2.2 for emo, w in agg.items()
        }

    def on_lyric_line(self, line_text: str, next_text: str = "") -> None:
        if line_text == self._last_lyric_text:
            return
        self._last_lyric_text = line_text
        cur_sc = score_text_keywords(line_text, extra_hints=False) if line_text else {}
        nxt_sc = score_text_keywords(next_text, extra_hints=False) if next_text else {}

        merged: dict[int, float] = dict(cur_sc)
        for emo, w in nxt_sc.items():
            merged[emo] = merged.get(emo, 0.0) + w * 0.30
        self._active_lyric_scores = merged

        # Inject active lyric emotions into decaying momentum memory (~10s half-life)
        for emo, w in cur_sc.items():
            prev = self._lyric_momentum.get(emo, 0.0)
            self._lyric_momentum[emo] = max(prev, min(3.5, w * 1.15))

    def decide(
        self,
        is_playing: bool,
        audio: AudioSnapshot,
        has_audio: bool,
        lyric_text: str,
        now_t: float,
    ) -> tuple[int, str]:
        dt = max(0.0, min(0.5, now_t - self._last_eval_t)) if self._last_eval_t > 0.0 else 0.02
        self._last_eval_t = now_t

        # Decay lyric emotional momentum smoothly over ~9 seconds
        if self._lyric_momentum:
            decay = math.exp(-dt / 9.0)
            self._lyric_momentum = {
                emo: val * decay
                for emo, val in self._lyric_momentum.items()
                if (val * decay) > 0.12
            }

        if not is_playing:
            if self.current_emo != EMO_NEUTRAL and (now_t - self._last_change_t) > 1.0:
                self.current_emo = EMO_NEUTRAL
                self.reason = "PAUSED"
                self._last_change_t = now_t
            return self.current_emo, "PAUSED"

        if has_audio and audio.energy < 0.025 and not lyric_text:
            if (now_t - self._last_change_t) > 3.5 and self.current_emo != EMO_NEUTRAL:
                self.current_emo = EMO_NEUTRAL
                self.reason = "QUIET"
                self._last_change_t = now_t
            return self.current_emo, "QUIET"

        scores: dict[int, float] = {}

        # Layer 1a: Active + lookahead lyric line sentiment (dominant expressive driver)
        for emo, w in self._active_lyric_scores.items():
            scores[emo] = scores.get(emo, 0.0) + w * 2.25

        # Layer 1b: Decaying emotional momentum from recent sung lines
        for emo, w in self._lyric_momentum.items():
            scores[emo] = scores.get(emo, 0.0) + w * 0.65

        # Layer 2a: Whole-song lyric baseline mood (prevents dropping to NEUTRAL on bridge lines)
        for emo, w in self._song_lyric_priors.items():
            scores[emo] = scores.get(emo, 0.0) + w * 0.42

        # Layer 2b: Track title / artist baseline mood
        for emo, w in self._title_priors.items():
            scores[emo] = scores.get(emo, 0.0) + w * 0.45

        # Layer 3: Real-time audio FFT features
        if has_audio and audio.energy >= 0.03:
            e = audio.energy
            b = audio.bass
            v = audio.vocal
            tr = audio.treble
            br = audio.beat_rate_hz

            if e > 0.78 and b > 0.72 and tr > 0.52 and br >= 1.8:
                scores[EMO_LAUGH] = scores.get(EMO_LAUGH, 0.0) + 1.4
                scores[EMO_ANGRY] = scores.get(EMO_ANGRY, 0.0) + (1.6 if tr > 0.70 else 0.5)
                scores[EMO_SURPRISED] = scores.get(EMO_SURPRISED, 0.0) + (1.1 if audio.beat else 0.2)
            elif e > 0.48 and br >= 1.25:
                scores[EMO_HAPPY] = scores.get(EMO_HAPPY, 0.0) + 1.3
                scores[EMO_WINK] = scores.get(EMO_WINK, 0.0) + 0.55
            elif e < 0.30 and br < 0.8:
                scores[EMO_SLEEPY] = scores.get(EMO_SLEEPY, 0.0) + 0.95
                scores[EMO_SAD] = scores.get(EMO_SAD, 0.0) + (1.05 if v > 0.20 else 0.55)
            elif v > 0.35 and b < 0.58:
                scores[EMO_LOVE] = scores.get(EMO_LOVE, 0.0) + 0.90
                scores[EMO_SAD] = scores.get(EMO_SAD, 0.0) + 0.75
            else:
                scores[EMO_HAPPY] = scores.get(EMO_HAPPY, 0.0) + 0.75

        if not scores:
            # Never default to a flat NEUTRAL while music is actively playing
            best_emo = EMO_HAPPY if (not has_audio or audio.energy > 0.38) else EMO_SLEEPY
            best_src = "AUDIO" if has_audio else "VIBE"
        else:
            # Slight stickiness bonus to prevent ping-ponging between close scores
            if self.current_emo in scores and self.current_emo != EMO_NEUTRAL:
                scores[self.current_emo] += 0.25
            best_emo = max(scores, key=lambda k: scores[k])
            if best_emo in self._active_lyric_scores or best_emo in self._lyric_momentum:
                best_src = "LYRIC"
            elif best_emo in self._song_lyric_priors or best_emo in self._title_priors:
                best_src = "SONG"
            else:
                best_src = "AUDIO"

        # Immediately leave NEUTRAL when a song starts playing; otherwise hold cleanly
        if self.current_emo == EMO_NEUTRAL and best_emo != EMO_NEUTRAL:
            min_hold = 0.0
        elif best_src == "LYRIC":
            min_hold = 1.6
        else:
            min_hold = 2.8

        if best_emo != self.current_emo and (now_t - self._last_change_t) >= min_hold:
            self.current_emo = best_emo
            self.reason = best_src
            self._last_change_t = now_t

        return self.current_emo, self.reason


# ============================================================================
# Main Bridge Loop
# ============================================================================
def list_audio_devices() -> None:
    print("\n--- Audio Input Devices (sounddevice) ---")
    try:
        import sounddevice as sd
        for idx, d in enumerate(sd.query_devices()):
            if d["max_input_channels"] > 0:
                host = sd.query_hostapis(d["hostapi"])["name"]
                print(f"  [{idx:2d}] {d['name']} ({host}, ch={d['max_input_channels']})")
    except Exception as exc:
        print(f"  sounddevice error: {exc}")

    print("\n--- WASAPI Loopback Devices (pyaudiowpatch) ---")
    try:
        import pyaudiowpatch as pyaudio  # type: ignore
        p = pyaudio.PyAudio()
        for dev in p.get_loopback_device_info_generator():
            print(f"  [{dev['index']:2d}] {dev['name']} (sr={int(dev['defaultSampleRate'])})")
        p.terminate()
    except ImportError:
        print("  pyaudiowpatch not installed (optional: pip install pyaudiowpatch)")
    except Exception as exc:
        print(f"  pyaudiowpatch error: {exc}")


async def run_sync(args: argparse.Namespace) -> int:
    lb = load_lyric_bridge(Path(args.lyric_bridge))
    if lb is None:
        print("[ERR] Could not load lyric_bridge.py; SMTC/Lyrics unavailable.")
        return 1

    audio = AudioAnalyzer(device_index=args.device, disabled=args.no_audio)
    audio.start()

    # Serial connection (reusing detect_esp32_port from lyric_bridge)
    ser: serial.Serial | None = None
    active_port = "DRY-RUN" if args.dry_run else "SEARCHING..."
    last_port_try = 0.0

    manager: Any = None
    clock = lb.PlaybackClock()
    lyrics_provider = lb.LyricsProvider()
    director = EmotionDirector()

    current_track_key: tuple[str, str] | None = None
    current_lyrics: Any = None
    lyrics_task: asyncio.Task[Any] | None = None
    smtc_state: Any = None
    last_smtc_t = 0.0
    last_entry_id: int | None = None
    last_seg_idx = -1

    tx_period = 1.0 / max(5.0, min(50.0, float(args.rate)))
    last_tx_t = 0.0
    last_print_t = 0.0

    def _ensure_serial(now_t: float) -> serial.Serial | None:
        nonlocal ser, active_port, last_port_try
        if args.dry_run:
            return None
        if ser is not None and ser.is_open:
            try:
                while ser.in_waiting > 0:
                    ser.readline()
            except serial.SerialException:
                ser.close()
                ser = None
                active_port = "DISCONNECTED"
            return ser
        if (now_t - last_port_try) < 2.0:
            return None
        last_port_try = now_t
        port = lb.detect_esp32_port(args.port)
        if not port:
            active_port = "NO PORT FOUND"
            return None
        try:
            s = serial.Serial(port, args.baud, timeout=0.02, write_timeout=0.3)
            time.sleep(1.6)  # allow ESP32 auto-reset on port open
            ser = s
            active_port = port
            return ser
        except serial.SerialException as exc:
            err_msg = str(exc)
            if "PermissionError" in err_msg or "Access is denied" in err_msg:
                active_port = f"{port} BUSY (Tutup Serial Monitor / lyric_bridge lain)"
            else:
                active_port = f"WAIT {port} ({exc})"
            return None

    def _bar(val: float, width: int = 12) -> str:
        fill = int(round(max(0.0, min(1.0, val)) * width))
        return "#" * fill + "-" * (width - fill)

    console = Console() if HAS_RICH else None
    live_ctx = Live(console=console, refresh_per_second=12, transient=False) if HAS_RICH else None
    if live_ctx is not None:
        live_ctx.__enter__()

    try:
        while True:
            now_mono = time.monotonic()
            now_utc = datetime.now(timezone.utc)

            # 1. Poll Windows SMTC every 200ms
            if (now_mono - last_smtc_t) >= 0.20:
                last_smtc_t = now_mono
                try:
                    manager, smtc_state = await lb.read_smtc_state(
                        manager, bitchord_only=args.bitchord_only
                    )
                except Exception:
                    manager = None
                    smtc_state = None

            # 2. Update playback clock & kick off async lyrics fetch on track change
            title = ""
            artist = ""
            is_playing = False
            pos_ms = 0
            lyric_text = ""

            if smtc_state is not None:
                title = smtc_state.title
                artist = smtc_state.artist
                is_playing = smtc_state.is_playing
                tkey = (title, artist)

                pos_ms = clock.update(
                    tkey,
                    smtc_state.raw_pos_ms,
                    smtc_state.duration_ms,
                    is_playing,
                    smtc_state.last_updated_utc,
                    now_utc,
                    now_mono,
                )

                if tkey != current_track_key:
                    current_track_key = tkey
                    current_lyrics = None
                    director.on_track_change(title, artist)
                    if lyrics_task is not None and not lyrics_task.done():
                        lyrics_task.cancel()
                    if not args.no_lyrics:
                        lyrics_task = asyncio.create_task(
                            asyncio.to_thread(
                                lyrics_provider.get_lyrics_sync,
                                title,
                                artist,
                                smtc_state.duration_ms,
                            )
                        )
            else:
                clock.reset()
                current_track_key = None
                current_lyrics = None

            if lyrics_task is not None and lyrics_task.done():
                try:
                    current_lyrics = lyrics_task.result()
                    if current_lyrics is not None and getattr(current_lyrics, "entries", None):
                        director.set_full_lyrics(current_lyrics.entries)
                except asyncio.CancelledError:
                    pass
                except Exception:
                    current_lyrics = None
                lyrics_task = None

            active_entry: Any = None
            next_lyric_text = ""
            if current_lyrics is not None and getattr(current_lyrics, "entries", None):
                entries = current_lyrics.entries
                active_entry = lb.find_active_entry(entries, pos_ms)
                if active_entry is not None:
                    lyric_text = active_entry.text or ""
                    try:
                        e_idx = entries.index(active_entry)
                        if e_idx + 1 < len(entries):
                            next_lyric_text = entries[e_idx + 1].text or ""
                    except ValueError:
                        pass

            if active_entry is not None:
                eid = id(active_entry)
                if eid != last_entry_id:
                    last_entry_id = eid
                    last_seg_idx = -1
            else:
                last_entry_id = None
                last_seg_idx = -1

            director.on_lyric_line(lyric_text, next_lyric_text)

            # 3. Read Audio Snapshot (or word-synced lyric pulse fallback if audio unavailable)
            snap = audio.read_snapshot()
            has_real_audio = not args.no_audio and not snap.source_name.startswith("UNAVAILABLE")
            if not has_real_audio and is_playing:
                pulse_t = now_mono * 2.2
                seg_hit = False
                seg_active = False
                if active_entry is not None and getattr(active_entry, "segments", None):
                    rel_ms = pos_ms - active_entry.start_ms
                    for s_i, seg in enumerate(active_entry.segments):
                        if seg.start_ms <= rel_ms <= seg.end_ms:
                            seg_active = True
                            if s_i != last_seg_idx:
                                last_seg_idx = s_i
                                seg_hit = True
                            break
                snap.energy = 0.55 if lyric_text else 0.25
                snap.vocal = (0.72 if seg_active else (0.52 * (0.5 + 0.5 * math.sin(pulse_t * 3.4)))) if lyric_text else 0.06
                snap.bass = 0.45 * (0.5 + 0.5 * math.sin(pulse_t))
                snap.beat = seg_hit

            # 4. Classify emotion & stream compact packet at ~25 Hz
            emo_idx, reason = director.decide(
                is_playing=is_playing,
                audio=snap,
                has_audio=has_real_audio,
                lyric_text=lyric_text,
                now_t=now_mono,
            )

            e_pct = int(round(max(0.0, min(1.0, snap.energy)) * 100))
            b_pct = int(round(max(0.0, min(1.0, snap.bass)) * 100))
            v_pct = int(round(max(0.0, min(1.0, snap.vocal)) * 100))
            beat_i = 1 if snap.beat else 0

            if (now_mono - last_tx_t) >= tx_period:
                last_tx_t = now_mono
                link = _ensure_serial(now_mono)
                pkt = f"M,{emo_idx},{e_pct},{b_pct},{v_pct},{beat_i}\n"
                if link is not None and link.is_open:
                    try:
                        link.write(pkt.encode("ascii"))
                    except serial.SerialException:
                        link.close()
                        ser = None
                        active_port = "DISCONNECTED"

            # 5. Update terminal UI
            if live_ctx is not None:
                tbl = Table.grid(padding=(0, 1))
                tbl.add_column(style="bold cyan", width=12)
                tbl.add_column()
                track_disp = f"{artist} - {title}" if artist else (title or "(No active media)")
                state_disp = "PLAYING" if is_playing else "PAUSED"
                lyric_disp = lyric_text if lyric_text else ("~ Instrumental ~" if current_lyrics else "-")
                beat_tag = " [bold red]*BEAT*[/]" if snap.beat else ""

                tbl.add_row("Serial", f"{active_port} @ {args.baud} bps ({args.rate} Hz)")
                tbl.add_row("Audio", snap.source_name)
                tbl.add_row("Track", f"[{state_disp}] {track_disp}")
                tbl.add_row("Lyric", lyric_disp)
                tbl.add_row("Emotion", f"[bold yellow]{EMO_NAMES[emo_idx].upper()}[/] ({reason}){beat_tag}")
                tbl.add_row("Bass", f"[{_bar(snap.bass)}] {b_pct:3d}%")
                tbl.add_row("Vocal", f"[{_bar(snap.vocal)}] {v_pct:3d}%")
                tbl.add_row("Energy", f"[{_bar(snap.energy)}] {e_pct:3d}%")
                live_ctx.update(Panel(tbl, title="BeatMo Music & Lyric Sync", border_style="green"))
            elif (now_mono - last_print_t) >= 0.5:
                last_print_t = now_mono
                print(
                    f"[{active_port}] {EMO_NAMES[emo_idx].upper()} ({reason}) | "
                    f"E={e_pct}% B={b_pct}% V={v_pct}% | {title or '-'} | {lyric_text or '-'}"
                )

            await asyncio.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        if live_ctx is not None:
            live_ctx.__exit__(None, None, None)
        audio.stop()
        if ser is not None and ser.is_open:
            try:
                ser.write(b"M,OFF\n")
                ser.flush()
                time.sleep(0.05)
                ser.close()
            except serial.SerialException:
                pass
        print("\n[EXIT] Music sync stopped cleanly.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="BeatMo Music & Lyric Emotion Sync Bridge")
    p.add_argument("--port", default="auto", help="ESP32 COM port (default: auto)")
    p.add_argument("--baud", type=int, default=115200, help="Serial baudrate (default: 115200)")
    p.add_argument("--rate", type=float, default=25.0, help="Packet rate Hz (default: 25)")
    p.add_argument("--device", type=int, default=None, help="sounddevice input index")
    p.add_argument("--dry-run", action="store_true", help="Run without opening Serial port")
    p.add_argument("--no-audio", action="store_true", help="Disable audio FFT capture")
    p.add_argument("--no-lyrics", action="store_true", help="Disable synced lyrics lookup")
    p.add_argument("--bitchord-only", action="store_true", help="Only follow BitChord SMTC session")
    p.add_argument("--list-devices", action="store_true", help="List audio devices and exit")
    p.add_argument(
        "--lyric-bridge",
        default=str(DEFAULT_LYRIC_BRIDGE),
        help="Path to lyric_bridge.py",
    )
    return p


def main() -> int:
    args = build_parser().parse_args()
    if args.list_devices:
        list_audio_devices()
        return 0
    return asyncio.run(run_sync(args))


if __name__ == "__main__":
    raise SystemExit(main())
