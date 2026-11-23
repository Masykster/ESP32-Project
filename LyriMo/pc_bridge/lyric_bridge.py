"""BitChord / Windows SMTC -> ESP32 OLED Lyric Bridge via USB Serial.

Reads the currently playing track and timeline from Windows System Media
Transport Controls (prioritizing BitChord Desktop), fetches word/syllable-synced
lyrics (local overrides -> disk cache -> BetterLyrics TTML -> LyricsPlus -> LRCLIB),
and streams compact line & syllable packets over USB Serial to the ESP32 OLED.
"""

from __future__ import annotations

import argparse
import asyncio
from bisect import bisect_right
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
import time
import unicodedata
from typing import Any
import xml.etree.ElementTree as ET

import requests
import serial
from serial.tools import list_ports

try:
    from winrt.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as SessionManager,
        GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
    )
    WINRT_IMPORT_ERROR: Exception | None = None
except Exception as exc:  # pragma: no cover - verified at runtime on Windows
    SessionManager = None
    PlaybackStatus = None
    WINRT_IMPORT_ERROR = exc


SCRIPT_DIR = Path(__file__).resolve().parent
LOCAL_LYRICS_DIR = SCRIPT_DIR / "lyrics_local"
CACHE_DIR = SCRIPT_DIR / ".lrc_cache"

BETTERLYRICS_URL = "https://lyrics-api.boidu.dev/getLyrics"
LYRICSPLUS_URLS = (
    "https://lyricsplus.prjktla.my.id/v2/lyrics/get",
)
LRCLIB_BASE_URL = "https://lrclib.net/api"
HTTP_HEADERS = {"User-Agent": "ESP32-OLED-Lyric-Bridge/2.0"}
HTTP_TIMEOUT_SEC = 4

DEFAULT_BAUD = 115200
POLL_INTERVAL_SEC = 0.20
PROGRESS_TX_INTERVAL_SEC = 0.35
TRACK_REFRESH_INTERVAL_SEC = 5.0
STANDBY_TX_INTERVAL_SEC = 2.0
NONE_DEBOUNCE_SEC = 1.5
ESP32_BOOT_DELAY_SEC = 1.8

MAX_WORD_PACKET_LEN = 460
MAX_SEGMENTS_PER_LINE = 44
INSTRUMENTAL_GAP_MS = 3000
INTRO_INSTRUMENTAL_MIN_MS = 2500

_CHAR_REPLACEMENTS = str.maketrans(
    {
        "’": "'",
        "‘": "'",
        "＇": "'",
        "‛": "'",
        "“": '"',
        "”": '"',
        "„": '"',
        "—": "-",
        "–": "-",
        "−": "-",
        "…": "...",
        "|": "/",
        "\u00a0": " ",
        "\u200b": "",
    }
)

_INLINE_TIME_TAG_RE = re.compile(r"<(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?>")
_LEADING_TIME_TAG_RE = re.compile(r"\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\]")
_OFFSET_TAG_RE = re.compile(r"^\[offset\s*:\s*([+-]?\d+)\]$", re.IGNORECASE)
_META_TAG_RE = re.compile(r"^\[[a-zA-Z]+:.*\]$")
_TITLE_NOISE_RE = re.compile(
    r"\s*[\(\[]"
    r"(?:official\s*(?:music\s*)?(?:video|audio|lyric\s*video|visualizer)|"
    r"lyrics?|audio|hd|4k|remaster(?:ed)?(?:\s*\d{4})?|feat\.?[^\)\]]*|ft\.?[^\)\]]*)"
    r"[\)\]]",
    re.IGNORECASE,
)
_FEAT_TAIL_RE = re.compile(r"\s+(?:feat\.|ft\.)\s+.*$", re.IGNORECASE)
_TOPIC_SUFFIX_RE = re.compile(r"\s*-\s*Topic$", re.IGNORECASE)
_FILENAME_SANITIZE_RE = re.compile(r'[<>:"/\\|?*]+')


@dataclass(frozen=True)
class MediaState:
    """Snapshot of the active Windows SMTC media session."""

    app_id: str
    title: str
    artist: str
    raw_pos_ms: int
    duration_ms: int
    is_playing: bool
    last_updated_utc: datetime | None


@dataclass(frozen=True)
class TimedSegment:
    """Word or syllable span relative to its parent LyricEntry start_ms."""

    start_ms: int
    end_ms: int
    char_end: int


@dataclass(frozen=True)
class LyricEntry:
    """Single synced lyric line (or empty instrumental break) with optional segments."""

    start_ms: int
    end_ms: int
    text: str
    segments: list[TimedSegment]


@dataclass
class LyricsResult:
    """Resolved lyric entries and status for a track."""

    status: str  # "word_synced", "line_synced", "instrumental", "unsynced", "not_found"
    entries: list[LyricEntry]


def _normalize_chars_preserve_spaces(text: str) -> str:
    """Fold Unicode characters to ASCII while preserving leading/trailing spaces."""
    if not text:
        return ""
    cleaned = _INLINE_TIME_TAG_RE.sub("", text)
    cleaned = cleaned.translate(_CHAR_REPLACEMENTS)
    cleaned = unicodedata.normalize("NFKD", cleaned)
    cleaned = cleaned.encode("ascii", "ignore").decode("ascii")
    return "".join(" " if (ord(ch) < 32 or ord(ch) == 127) else ch for ch in cleaned)


def sanitize_oled_text(text: str, max_len: int = 120) -> str:
    """Normalize text to clean single-line ASCII safe for U8g2 OLED & serial."""
    if not text:
        return ""
    cleaned = " ".join(_normalize_chars_preserve_spaces(text).split())
    if max_len > 0 and len(cleaned) > max_len:
        if max_len >= 4:
            cleaned = cleaned[: max_len - 3].rstrip() + "..."
        else:
            cleaned = cleaned[:max_len]
    return cleaned


def _parse_frac_ms(frac: str | None) -> int:
    if not frac:
        return 0
    if len(frac) == 1:
        return int(frac) * 100
    if len(frac) == 2:
        return int(frac) * 10
    return int(frac[:3])


def parse_ttml_time_ms(value: str | None) -> int | None:
    """Parse TTML timestamp formats: '12.819', '1:46.299', '00:00:00.160', '12s', '500ms'."""
    if not value:
        return None
    raw = value.strip()
    if not raw:
        return None
    if raw.endswith("ms"):
        try:
            return max(0, int(round(float(raw[:-2]))))
        except ValueError:
            return None
    if raw.endswith("s"):
        try:
            return max(0, int(round(float(raw[:-1]) * 1000.0)))
        except ValueError:
            return None

    parts = raw.split(":")
    try:
        if len(parts) == 1:
            return max(0, int(round(float(parts[0]) * 1000.0)))
        if len(parts) == 2:
            mins = int(parts[0])
            secs = float(parts[1])
            return max(0, int(round((mins * 60.0 + secs) * 1000.0)))
        if len(parts) == 3:
            hours = int(parts[0])
            mins = int(parts[1])
            secs = float(parts[2])
            return max(0, int(round((hours * 3600.0 + mins * 60.0 + secs) * 1000.0)))
    except ValueError:
        return None
    return None


def _build_text_and_segments(
    pieces: list[tuple[int, int, str]],
    line_start_ms: int,
    max_len: int = 150,
) -> tuple[str, list[TimedSegment]]:
    """Assemble sanitized OLED text and monotonic relative segments from timed pieces."""
    out_chars: list[str] = []
    segments: list[TimedSegment] = []
    last_rel_start = 0

    for abs_start, abs_end, raw_piece in pieces:
        normalized = _normalize_chars_preserve_spaces(raw_piece)
        piece_chars: list[str] = []
        for ch in normalized:
            if ch == " ":
                prev_ch = (
                    piece_chars[-1]
                    if piece_chars
                    else (out_chars[-1] if out_chars else "")
                )
                if prev_ch and prev_ch != " ":
                    piece_chars.append(" ")
            else:
                piece_chars.append(ch)

        if not piece_chars:
            continue

        remaining = max_len - len(out_chars)
        if remaining <= 0:
            break
        if len(piece_chars) > remaining:
            piece_chars = piece_chars[:remaining]

        out_chars.extend(piece_chars)
        char_end = len(out_chars)
        rel_start = max(last_rel_start, abs_start - line_start_ms)
        rel_end = max(rel_start + 30, abs_end - line_start_ms)
        last_rel_start = rel_start

        if segments and char_end == segments[-1].char_end:
            continue
        if segments and segments[-1].start_ms == rel_start and segments[-1].end_ms == rel_end:
            segments[-1] = TimedSegment(rel_start, rel_end, char_end)
        else:
            segments.append(TimedSegment(rel_start, rel_end, char_end))

    while out_chars and out_chars[-1] == " ":
        out_chars.pop()
    full_text = "".join(out_chars)
    if not full_text:
        return "", []

    clamped: list[TimedSegment] = []
    for seg in segments:
        ce = min(seg.char_end, len(full_text))
        if clamped and ce <= clamped[-1].char_end:
            clamped[-1] = TimedSegment(
                clamped[-1].start_ms, max(clamped[-1].end_ms, seg.end_ms), clamped[-1].char_end
            )
            continue
        if ce > 0:
            clamped.append(TimedSegment(seg.start_ms, seg.end_ms, ce))
    if clamped and clamped[-1].char_end < len(full_text):
        last = clamped[-1]
        clamped[-1] = TimedSegment(last.start_ms, last.end_ms, len(full_text))
    return full_text, clamped


def build_proportional_segments(text: str, total_dur_ms: int) -> list[TimedSegment]:
    """Generate smooth word-by-word proportional segments across a line's duration."""
    if not text:
        return []
    dur = max(300, int(total_dur_ms))
    tokens = re.findall(r"\S+\s*", text)
    if not tokens:
        return []
    if len(tokens) == 1:
        return [TimedSegment(0, dur, len(text))]

    weights = [max(1, len(tok.strip())) for tok in tokens]
    total_weight = sum(weights)
    segments: list[TimedSegment] = []
    cursor_ms = 0
    char_cursor = 0
    for idx, (tok, w) in enumerate(zip(tokens, weights)):
        char_cursor += len(tok)
        if idx == len(tokens) - 1:
            end_ms = dur
            char_cursor = len(text)
        else:
            span = max(80, int(round(dur * (w / total_weight))))
            end_ms = min(dur, cursor_ms + span)
        segments.append(TimedSegment(cursor_ms, max(cursor_ms + 30, end_ms), char_cursor))
        cursor_ms = min(dur, end_ms)
    return segments


def _estimate_line_sing_duration_ms(text: str, gap_to_next_ms: int | None) -> int:
    """Estimate singing window so karaoke sweep finishes naturally before long pauses."""
    estimated = max(1600, len(text) * 85)
    if gap_to_next_ms is None:
        return max(2400, min(6500, estimated))
    if gap_to_next_ms <= 0:
        return 1000
    if gap_to_next_ms > estimated + INSTRUMENTAL_GAP_MS:
        return estimated
    return max(400, gap_to_next_ms)


def _insert_instrumental_gaps(entries: list[LyricEntry]) -> list[LyricEntry]:
    """Normalize ordering and insert empty instrumental markers across long gaps."""
    if not entries:
        return []
    entries_sorted = sorted(entries, key=lambda e: e.start_ms)
    result: list[LyricEntry] = []

    if entries_sorted[0].start_ms >= INTRO_INSTRUMENTAL_MIN_MS and entries_sorted[0].text:
        result.append(LyricEntry(0, entries_sorted[0].start_ms, "", []))

    for idx, entry in enumerate(entries_sorted):
        result.append(entry)
        if not entry.text:
            continue
        if idx + 1 < len(entries_sorted):
            next_entry = entries_sorted[idx + 1]
            if next_entry.text and (next_entry.start_ms - entry.end_ms) >= INSTRUMENTAL_GAP_MS:
                result.append(LyricEntry(entry.end_ms, next_entry.start_ms, "", []))
        else:
            # Show instrumental note after the final sung line ends
            result.append(LyricEntry(entry.end_ms + 1200, entry.end_ms + 600000, "", []))
    return result


def parse_lrc(lrc_text: str) -> list[tuple[int, str]]:
    """Parse LRC text into sorted (timestamp_ms, raw_lyric_text) tuples."""
    if not lrc_text:
        return []

    offset_ms = 0
    raw_entries: list[tuple[int, str]] = []

    for raw_line in lrc_text.lstrip("\ufeff").splitlines():
        line = raw_line.strip()
        if not line:
            continue

        offset_match = _OFFSET_TAG_RE.match(line)
        if offset_match:
            offset_ms = int(offset_match.group(1))
            continue

        pos = 0
        timestamps: list[int] = []
        while pos < len(line):
            tag_match = _LEADING_TIME_TAG_RE.match(line, pos)
            if not tag_match:
                break
            minutes = int(tag_match.group(1))
            seconds = int(tag_match.group(2))
            millis = _parse_frac_ms(tag_match.group(3))
            timestamps.append((minutes * 60 + seconds) * 1000 + millis)
            pos = tag_match.end()

        if not timestamps:
            if _META_TAG_RE.match(line):
                continue
            continue

        lyric_body = _INLINE_TIME_TAG_RE.sub("", line[pos:]).strip()
        for ts in timestamps:
            raw_entries.append((ts, lyric_body))

    adjusted = [(max(0, ts - offset_ms), text) for ts, text in raw_entries]
    adjusted.sort(key=lambda item: item[0])
    return adjusted


def _parse_enhanced_body(
    body: str, line_start_ms: int, next_line_start_ms: int | None, offset_ms: int
) -> list[tuple[int, int, str]] | None:
    """Parse Enhanced LRC inline <mm:ss.xx> tags if present."""
    matches = list(_INLINE_TIME_TAG_RE.finditer(body))
    if not matches:
        return None

    raw_chunks: list[tuple[int, str]] = []
    for idx, m in enumerate(matches):
        ts = (int(m.group(1)) * 60 + int(m.group(2))) * 1000 + _parse_frac_ms(m.group(3))
        ts = max(0, ts - offset_ms)
        text_start = m.end()
        text_end = matches[idx + 1].start() if idx + 1 < len(matches) else len(body)
        chunk_text = body[text_start:text_end]
        if chunk_text:
            raw_chunks.append((ts, chunk_text))

    if not raw_chunks:
        return None

    pieces: list[tuple[int, int, str]] = []
    for idx, (ts, chunk_text) in enumerate(raw_chunks):
        if idx + 1 < len(raw_chunks):
            next_ts = raw_chunks[idx + 1][0]
        elif next_line_start_ms is not None and next_line_start_ms > ts:
            next_ts = min(next_line_start_ms, ts + max(600, len(chunk_text.strip()) * 150))
        else:
            next_ts = max(ts + 600, line_start_ms + 800)
        pieces.append((ts, max(ts + 80, next_ts), chunk_text))
    return pieces


def parse_lrc_entries(lrc_text: str) -> tuple[list[LyricEntry], bool]:
    """Parse standard or Enhanced LRC into LyricEntry list. Returns (entries, is_word_synced)."""
    if not lrc_text:
        return [], False

    offset_ms = 0
    raw_lines: list[tuple[int, str]] = []
    for raw_line in lrc_text.lstrip("\ufeff").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        offset_match = _OFFSET_TAG_RE.match(line)
        if offset_match:
            offset_ms = int(offset_match.group(1))
            continue

        pos = 0
        timestamps: list[int] = []
        while pos < len(line):
            tag_match = _LEADING_TIME_TAG_RE.match(line, pos)
            if not tag_match:
                break
            ts = (int(tag_match.group(1)) * 60 + int(tag_match.group(2))) * 1000 + _parse_frac_ms(
                tag_match.group(3)
            )
            timestamps.append(ts)
            pos = tag_match.end()

        if not timestamps:
            continue
        body = line[pos:].strip()
        for ts in timestamps:
            raw_lines.append((max(0, ts - offset_ms), body))

    raw_lines.sort(key=lambda item: item[0])
    if not raw_lines:
        return [], False

    has_enhanced = False
    entries: list[LyricEntry] = []
    for idx, (start_ms, body) in enumerate(raw_lines):
        next_start = raw_lines[idx + 1][0] if idx + 1 < len(raw_lines) else None
        if not body:
            end_ms = next_start if next_start is not None else start_ms + 3000
            entries.append(LyricEntry(start_ms, max(start_ms, end_ms), "", []))
            continue

        enhanced_pieces = _parse_enhanced_body(body, start_ms, next_start, offset_ms)
        if enhanced_pieces:
            has_enhanced = True
            full_text, segs = _build_text_and_segments(enhanced_pieces, start_ms)
            if not full_text:
                continue
            rel_end = segs[-1].end_ms if segs else 1000
            end_ms = start_ms + rel_end
            if next_start is not None and next_start > start_ms:
                end_ms = min(end_ms, next_start)
            entries.append(LyricEntry(start_ms, max(start_ms + 100, end_ms), full_text, segs))
            continue

        clean_text = sanitize_oled_text(body, max_len=150)
        if not clean_text:
            clean_text = "(Non-Latin lyric)"
        gap = (next_start - start_ms) if next_start is not None else None
        sing_dur = _estimate_line_sing_duration_ms(clean_text, gap)
        segs = build_proportional_segments(clean_text, sing_dur)
        entries.append(LyricEntry(start_ms, start_ms + sing_dur, clean_text, segs))

    return _insert_instrumental_gaps(entries), has_enhanced


def _xml_local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].lower()


def _xml_attr(elem: ET.Element, target_name: str) -> str | None:
    target_lower = target_name.lower()
    for key, val in elem.attrib.items():
        if _xml_local(key) == target_lower:
            return val
    return None


def _collect_ttml_spans(
    node: ET.Element,
    skip_bg: bool,
    parent_begin: int,
    parent_end: int,
    out_pieces: list[tuple[int, int, str]],
) -> None:
    """Recursively collect timed leaf spans and inter-span spaces from a TTML <p>."""
    if node.text:
        if out_pieces and not node.text.strip():
            prev_s, prev_e, prev_t = out_pieces[-1]
            out_pieces[-1] = (prev_s, prev_e, prev_t + node.text)
        elif node.text.strip():
            out_pieces.append((parent_begin, parent_end, node.text))

    for child in node:
        tag_name = _xml_local(child.tag)
        role = (_xml_attr(child, "role") or "").lower()
        skip_child = role in ("x-translation", "x-roman") or (skip_bg and role == "x-bg")

        if not skip_child and tag_name == "span":
            c_begin = parse_ttml_time_ms(_xml_attr(child, "begin"))
            c_end = parse_ttml_time_ms(_xml_attr(child, "end"))
            eff_begin = c_begin if c_begin is not None else parent_begin
            eff_end = c_end if c_end is not None else parent_end

            has_child_spans = any(_xml_local(gc.tag) == "span" for gc in child)
            if has_child_spans:
                _collect_ttml_spans(child, skip_bg, eff_begin, eff_end, out_pieces)
            else:
                leaf_text = "".join(child.itertext())
                if leaf_text:
                    out_pieces.append((eff_begin, max(eff_begin + 30, eff_end), leaf_text))

        if child.tail:
            if out_pieces:
                prev_s, prev_e, prev_t = out_pieces[-1]
                out_pieces[-1] = (prev_s, prev_e, prev_t + child.tail)
            elif child.tail.strip():
                out_pieces.append((parent_begin, parent_end, child.tail))


def parse_ttml(ttml_text: str) -> tuple[list[LyricEntry], bool]:
    """Parse Apple Music / BetterLyrics TTML XML into LyricEntry list."""
    if not ttml_text or "<" not in ttml_text:
        return [], False
    try:
        root = ET.fromstring(ttml_text)
    except ET.ParseError:
        return [], False

    timing_attr = (_xml_attr(root, "timing") or "").lower()
    entries: list[LyricEntry] = []
    any_word_spans = timing_attr == "word"

    p_elements = [el for el in root.iter() if _xml_local(el.tag) == "p"]
    parsed_raw: list[tuple[int, int | None, ET.Element]] = []
    for p in p_elements:
        begin_ms = parse_ttml_time_ms(_xml_attr(p, "begin"))
        if begin_ms is None:
            continue
        end_ms = parse_ttml_time_ms(_xml_attr(p, "end"))
        parsed_raw.append((begin_ms, end_ms, p))

    for idx, (begin_ms, end_ms, p) in enumerate(parsed_raw):
        next_begin = parsed_raw[idx + 1][0] if idx + 1 < len(parsed_raw) else None
        eff_end = end_ms
        if eff_end is None or eff_end <= begin_ms:
            gap = (next_begin - begin_ms) if next_begin is not None else None
            eff_end = begin_ms + (gap if gap and gap > 0 else 3000)

        has_timed_span = any(
            _xml_local(el.tag) == "span" and _xml_attr(el, "begin") is not None
            for el in p.iter()
        )
        if has_timed_span:
            any_word_spans = True
            pieces: list[tuple[int, int, str]] = []
            _collect_ttml_spans(p, True, begin_ms, eff_end, pieces)
            full_text, segs = _build_text_and_segments(pieces, begin_ms)
            if not full_text:
                pieces_bg: list[tuple[int, int, str]] = []
                _collect_ttml_spans(p, False, begin_ms, eff_end, pieces_bg)
                full_text, segs = _build_text_and_segments(pieces_bg, begin_ms)
            if not full_text:
                continue
            final_end = max(eff_end, begin_ms + (segs[-1].end_ms if segs else 300))
            entries.append(LyricEntry(begin_ms, final_end, full_text, segs))
        else:
            raw_text = "".join(p.itertext())
            clean_text = sanitize_oled_text(raw_text, max_len=150)
            if not clean_text:
                if raw_text.strip():
                    clean_text = "(Non-Latin lyric)"
                else:
                    continue
            segs = build_proportional_segments(clean_text, max(400, eff_end - begin_ms))
            entries.append(LyricEntry(begin_ms, eff_end, clean_text, segs))

    return _insert_instrumental_gaps(entries), any_word_spans


def _align_syllables_with_line(
    line_text: str, syllabus: list[dict[str, Any]]
) -> list[tuple[int, int, str]]:
    """Reconstruct timed pieces from LyricsPlus syllabus items, preserving word spaces."""
    pieces: list[tuple[int, int, str]] = []
    joined_syl = "".join(str(s.get("text", "")) for s in syllabus)
    has_spaces_in_syl = " " in joined_syl
    cursor = 0

    for item in syllabus:
        txt = str(item.get("text", ""))
        if not txt:
            continue
        t_ms = int(item.get("time", 0))
        d_ms = max(30, int(item.get("duration", 0)))

        if not has_spaces_in_syl and line_text:
            pos = line_text.find(txt, cursor)
            if pos != -1:
                cursor = pos + len(txt)
                while cursor < len(line_text) and line_text[cursor] == " ":
                    txt += " "
                    cursor += 1
        pieces.append((t_ms, t_ms + d_ms, txt))
    return pieces


def parse_lyricsplus_json(payload: dict[str, Any]) -> tuple[list[LyricEntry], bool]:
    """Parse LyricsPlus /v2/lyrics/get JSON payload into LyricEntry list."""
    raw_lines = payload.get("lyrics")
    if not isinstance(raw_lines, list) or not raw_lines:
        return [], False

    is_word = str(payload.get("type", "")).lower() == "word"
    entries: list[LyricEntry] = []

    for idx, item in enumerate(raw_lines):
        if not isinstance(item, dict):
            continue
        start_ms = int(item.get("time", 0))
        dur_ms = int(item.get("duration", 0))
        next_time = (
            int(raw_lines[idx + 1].get("time", 0))
            if idx + 1 < len(raw_lines) and isinstance(raw_lines[idx + 1], dict)
            else None
        )
        end_ms = start_ms + dur_ms if dur_ms > 0 else (next_time or start_ms + 3000)

        line_text = str(item.get("text", ""))
        syllabus = item.get("syllabus")
        if isinstance(syllabus, list) and syllabus:
            is_word = True
            pieces = _align_syllables_with_line(line_text, syllabus)
            full_text, segs = _build_text_and_segments(pieces, start_ms)
            if full_text:
                final_end = max(end_ms, start_ms + (segs[-1].end_ms if segs else 300))
                entries.append(LyricEntry(start_ms, final_end, full_text, segs))
                continue

        clean_text = sanitize_oled_text(line_text, max_len=150)
        if not clean_text:
            if line_text.strip():
                clean_text = "(Non-Latin lyric)"
            else:
                continue
        segs = build_proportional_segments(clean_text, max(400, end_ms - start_ms))
        entries.append(LyricEntry(start_ms, max(start_ms + 400, end_ms), clean_text, segs))

    return _insert_instrumental_gaps(entries), is_word


def find_active_lyric(
    lines: list[tuple[int, str]], pos_ms: int, offset_ms: int = 0
) -> str:
    """Return the active lyric string for pos_ms (plus optional manual offset)."""
    if not lines:
        return ""
    query_ms = max(0, int(pos_ms) + int(offset_ms))
    timestamps = [ts for ts, _ in lines]
    idx = bisect_right(timestamps, query_ms)
    if idx <= 0:
        return ""
    return lines[idx - 1][1]


def find_active_entry(
    entries: list[LyricEntry], pos_ms: int, offset_ms: int = 0
) -> LyricEntry | None:
    """Return active LyricEntry for pos_ms (plus optional manual offset)."""
    if not entries:
        return None
    query_ms = max(0, int(pos_ms) + int(offset_ms))
    starts = [e.start_ms for e in entries]
    idx = bisect_right(starts, query_ms)
    if idx <= 0:
        return None
    return entries[idx - 1]


def _coalesce_segments(
    segments: list[TimedSegment], target_count: int
) -> list[TimedSegment]:
    """Merge adjacent segments in pairs until segment count <= target_count."""
    current = list(segments)
    while len(current) > target_count and len(current) > 1:
        merged: list[TimedSegment] = []
        i = 0
        while i < len(current):
            if i + 1 < len(current):
                a, b = current[i], current[i + 1]
                merged.append(TimedSegment(a.start_ms, max(a.end_ms, b.end_ms), b.char_end))
                i += 2
            else:
                merged.append(current[i])
                i += 1
        current = merged
    return current


def format_word_packet(
    entry: LyricEntry,
    offset_ms: int = 0,
    max_packet_len: int = MAX_WORD_PACKET_LEN,
) -> str:
    """Serialize a LyricEntry to compact W| or L| serial protocol line."""
    if not entry.text:
        return "L|"

    eff_start = max(0, entry.start_ms - offset_ms)
    eff_end = max(eff_start + 100, entry.end_ms - offset_ms)
    segs = entry.segments

    if not segs:
        return f"L|{entry.text}"

    cap = MAX_SEGMENTS_PER_LINE
    while True:
        trimmed = _coalesce_segments(segs, cap) if len(segs) > cap else segs
        seg_str = ",".join(f"{s.start_ms}:{s.end_ms}:{s.char_end}" for s in trimmed)
        packet = f"W|{eff_start}|{eff_end}|{entry.text}|{seg_str}"
        if len(packet) <= max_packet_len:
            return packet
        if len(trimmed) <= 1:
            return f"L|{entry.text}"
        cap = max(1, len(trimmed) // 2)


def clean_track_query(title: str, artist: str) -> tuple[str, str]:
    """Strip common video/audio tags from track title and YouTube Topic from artist."""
    clean_title = _TITLE_NOISE_RE.sub("", title or "")
    clean_title = _FEAT_TAIL_RE.sub("", clean_title)
    clean_title = " ".join(clean_title.split()).strip(" -")

    clean_artist = _TOPIC_SUFFIX_RE.sub("", artist or "")
    clean_artist = " ".join(clean_artist.split()).strip()
    return clean_title or (title or "").strip(), clean_artist


def compute_live_position_ms(
    raw_pos_ms: int,
    duration_ms: int,
    is_playing: bool,
    last_updated_utc: datetime | None,
    now_utc: datetime,
    fallback_elapsed_ms: int = 0,
) -> int:
    """Interpolate playback position using SMTC last_updated_time or monotonic fallback."""
    pos = max(0, int(raw_pos_ms))
    if is_playing:
        now_aware = (
            now_utc
            if now_utc.tzinfo is not None
            else now_utc.replace(tzinfo=timezone.utc)
        )
        used_smtc_clock = False
        if last_updated_utc is not None and last_updated_utc.year >= 2000:
            lu_aware = (
                last_updated_utc.astimezone(timezone.utc)
                if last_updated_utc.tzinfo is not None
                else last_updated_utc.replace(tzinfo=timezone.utc)
            )
            delta_ms = int((now_aware - lu_aware).total_seconds() * 1000)
            if 0 <= delta_ms <= 300_000:
                pos += delta_ms
                used_smtc_clock = True
        if not used_smtc_clock and fallback_elapsed_ms > 0:
            pos += int(fallback_elapsed_ms)

    if duration_ms > 0:
        pos = min(pos, int(duration_ms))
    return max(0, pos)


class PlaybackClock:
    """Stateful playback clock resilient to BitChord SMTC pos=0 on pause and stale timestamps."""

    def __init__(self) -> None:
        self.track_id: tuple[str, str] | None = None
        self.pos_ms: int = 0
        self.last_mono: float = 0.0
        self.last_raw_pos_ms: int = 0
        self.last_updated_utc: datetime | None = None
        self.was_playing: bool = False

    def reset(self) -> None:
        self.track_id = None
        self.pos_ms = 0
        self.last_mono = 0.0
        self.last_raw_pos_ms = 0
        self.last_updated_utc = None
        self.was_playing = False

    @staticmethod
    def _fresh_delta_ms(
        last_updated_utc: datetime | None,
        now_utc: datetime,
        max_ms: int,
    ) -> int | None:
        if last_updated_utc is None or last_updated_utc.year < 2000:
            return None
        now_aware = (
            now_utc
            if now_utc.tzinfo is not None
            else now_utc.replace(tzinfo=timezone.utc)
        )
        lu_aware = (
            last_updated_utc.astimezone(timezone.utc)
            if last_updated_utc.tzinfo is not None
            else last_updated_utc.replace(tzinfo=timezone.utc)
        )
        delta_ms = int((now_aware - lu_aware).total_seconds() * 1000)
        if 0 <= delta_ms <= max_ms:
            return delta_ms
        return None

    def update(
        self,
        track_id: tuple[str, str],
        raw_pos_ms: int,
        duration_ms: int,
        is_playing: bool,
        last_updated_utc: datetime | None,
        now_utc: datetime,
        now_mono: float,
    ) -> int:
        dur = max(0, int(duration_ms))
        raw_pos = max(0, int(raw_pos_ms))
        elapsed_ms = (
            max(0, int(round((now_mono - self.last_mono) * 1000)))
            if self.last_mono > 0.0
            else 0
        )

        if self.track_id is None:
            pos = raw_pos
            if is_playing:
                max_d = min(300_000, dur) if dur > 0 else 300_000
                delta = self._fresh_delta_ms(last_updated_utc, now_utc, max_d)
                if delta is not None:
                    pos += delta
            self.pos_ms = pos
        elif track_id != self.track_id:
            pos = (
                raw_pos
                if (raw_pos > 0 and (dur == 0 or raw_pos < max(1000, dur - 1000)))
                else 0
            )
            if is_playing and pos == 0 and last_updated_utc != self.last_updated_utc:
                delta = self._fresh_delta_ms(last_updated_utc, now_utc, 3_000)
                if delta is not None:
                    pos += delta
            self.pos_ms = pos
        else:
            raw_changed = raw_pos != self.last_raw_pos_ms
            upd_changed = last_updated_utc != self.last_updated_utc

            if self.was_playing and not is_playing:
                self.pos_ms += elapsed_ms
                if raw_pos > 0 and (raw_changed or abs(raw_pos - self.pos_ms) < 5000):
                    self.pos_ms = raw_pos
            elif not self.was_playing and not is_playing:
                if raw_changed and raw_pos > 0:
                    self.pos_ms = raw_pos
                elif raw_changed and raw_pos == 0 and self.last_raw_pos_ms > 0 and upd_changed:
                    self.pos_ms = 0
            elif not self.was_playing and is_playing:
                if raw_pos > 0 and (raw_changed or self.pos_ms == 0):
                    delta = self._fresh_delta_ms(last_updated_utc, now_utc, 2_000) or 0
                    self.pos_ms = raw_pos + delta
            else:
                if raw_pos > 0 and (raw_changed or upd_changed):
                    delta = self._fresh_delta_ms(last_updated_utc, now_utc, 2_000) or 0
                    self.pos_ms = raw_pos + delta
                elif raw_changed and raw_pos == 0 and self.last_raw_pos_ms > 0:
                    self.pos_ms = 0
                elif (
                    raw_pos == 0
                    and dur > 0
                    and self.pos_ms >= max(0, dur - 1500)
                    and upd_changed
                    and self._fresh_delta_ms(last_updated_utc, now_utc, 2_000) is not None
                ):
                    self.pos_ms = 0
                else:
                    self.pos_ms += elapsed_ms

        if dur > 0:
            self.pos_ms = min(self.pos_ms, dur)
        self.pos_ms = max(0, self.pos_ms)

        self.track_id = track_id
        self.last_mono = now_mono
        self.last_raw_pos_ms = raw_pos
        self.last_updated_utc = last_updated_utc
        self.was_playing = is_playing
        return self.pos_ms


class LyricsProvider:
    """Fetches and caches word/line-synced lyrics from local files, BetterLyrics, LyricsPlus, or LRCLIB."""

    def __init__(
        self,
        local_dir: Path = LOCAL_LYRICS_DIR,
        cache_dir: Path = CACHE_DIR,
    ) -> None:
        self.local_dir = local_dir
        self.cache_dir = cache_dir
        self._memory_cache: dict[str, LyricsResult] = {}

    @staticmethod
    def _cache_key(title: str, artist: str, duration_ms: int = 0) -> str:
        raw = f"v3||{artist.lower().strip()}||{title.lower().strip()}"
        return hashlib.sha1(raw.encode("utf-8")).hexdigest()

    def _find_local_lrc(self, title: str, artist: str) -> Path | None:
        if not self.local_dir.exists():
            return None
        clean_title, clean_artist = clean_track_query(title, artist)
        candidates: list[str] = []
        for a, t in ((artist, title), (clean_artist, clean_title)):
            if a and t:
                candidates.append(f"{a} - {t}")
            if t:
                candidates.append(t)

        normalized_targets = {
            _FILENAME_SANITIZE_RE.sub("", c).strip().lower(): True
            for c in candidates
            if c.strip()
        }
        for lrc_path in sorted(self.local_dir.glob("*.lrc")):
            if lrc_path.stem.strip().lower() in normalized_targets:
                return lrc_path
        return None

    def _read_disk_cache(self, key: str) -> LyricsResult | None:
        cache_file = self.cache_dir / f"{key}_v3.json"
        if not cache_file.exists():
            return None
        try:
            payload = json.loads(cache_file.read_text(encoding="utf-8"))
            entries: list[LyricEntry] = []
            for row in payload.get("entries", []):
                segs = [
                    TimedSegment(int(s[0]), int(s[1]), int(s[2]))
                    for s in row[3]
                ]
                entries.append(
                    LyricEntry(int(row[0]), int(row[1]), str(row[2]), segs)
                )
            return LyricsResult(status=str(payload["status"]), entries=entries)
        except Exception:
            return None

    def _write_disk_cache(self, key: str, result: LyricsResult) -> None:
        try:
            self.cache_dir.mkdir(parents=True, exist_ok=True)
            cache_file = self.cache_dir / f"{key}_v3.json"
            payload = {
                "status": result.status,
                "entries": [
                    [
                        e.start_ms,
                        e.end_ms,
                        e.text,
                        [[s.start_ms, s.end_ms, s.char_end] for s in e.segments],
                    ]
                    for e in result.entries
                ],
            }
            cache_file.write_text(
                json.dumps(payload, ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except Exception:
            pass

    @staticmethod
    def _pick_lrclib_match(
        items: list[dict[str, Any]], duration_sec: int
    ) -> dict[str, Any] | None:
        synced = [it for it in items if isinstance(it, dict) and it.get("syncedLyrics")]
        if not synced:
            return None
        if duration_sec <= 0:
            return synced[0]
        within_tol = [
            it
            for it in synced
            if it.get("duration") is not None
            and abs(float(it["duration"]) - duration_sec) <= 15.0
        ]
        pool = within_tol if within_tol else synced
        return min(
            pool,
            key=lambda it: abs(float(it.get("duration") or duration_sec) - duration_sec),
        )

    def _query_betterlyrics(
        self, queries: list[tuple[str, str]], duration_sec: int
    ) -> tuple[LyricsResult | None, bool]:
        had_network_error = False
        for q_title, q_artist in queries:
            if not q_title:
                continue
            params: dict[str, Any] = {"s": q_title}
            if q_artist:
                params["a"] = q_artist
            if duration_sec > 0:
                params["d"] = duration_sec
            try:
                resp = requests.get(
                    BETTERLYRICS_URL,
                    params=params,
                    headers=HTTP_HEADERS,
                    timeout=HTTP_TIMEOUT_SEC,
                )
                if resp.status_code != 200:
                    continue
                data = resp.json()
                ttml = data.get("ttml") or ""
                if ttml:
                    entries, is_word = parse_ttml(ttml)
                    if entries:
                        status = "word_synced" if is_word else "line_synced"
                        return LyricsResult(status, entries), had_network_error
            except (requests.RequestException, ValueError):
                had_network_error = True
        return None, had_network_error

    def _query_lyricsplus(
        self, queries: list[tuple[str, str]], duration_sec: int
    ) -> tuple[LyricsResult | None, bool]:
        had_network_error = False
        for base_url in LYRICSPLUS_URLS:
            for q_title, q_artist in queries:
                if not q_title:
                    continue
                params: dict[str, Any] = {"title": q_title}
                if q_artist:
                    params["artist"] = q_artist
                if duration_sec > 0:
                    params["duration"] = duration_sec
                try:
                    resp = requests.get(
                        base_url,
                        params=params,
                        headers=HTTP_HEADERS,
                        timeout=HTTP_TIMEOUT_SEC,
                    )
                    if resp.status_code != 200:
                        continue
                    data = resp.json()
                    if isinstance(data, dict):
                        entries, is_word = parse_lyricsplus_json(data)
                        if entries:
                            status = "word_synced" if is_word else "line_synced"
                            return LyricsResult(status, entries), had_network_error
                except (requests.RequestException, ValueError):
                    had_network_error = True
        return None, had_network_error

    def _query_lrclib(
        self, queries: list[tuple[str, str]], duration_sec: int
    ) -> tuple[LyricsResult, bool]:
        """Query LRCLIB as line/enhanced-LRC fallback. Returns (result, should_cache)."""
        try:
            for q_title, q_artist in queries:
                if not q_title:
                    continue
                params: dict[str, Any] = {"track_name": q_title}
                if q_artist:
                    params["artist_name"] = q_artist
                if duration_sec > 0:
                    params["duration"] = duration_sec

                resp = requests.get(
                    f"{LRCLIB_BASE_URL}/get",
                    params=params,
                    headers=HTTP_HEADERS,
                    timeout=HTTP_TIMEOUT_SEC,
                )
                if resp.status_code == 200:
                    data = resp.json()
                    if data.get("instrumental"):
                        return (
                            LyricsResult(
                                "instrumental",
                                [LyricEntry(0, 600_000, "Instrumental", [])],
                            ),
                            True,
                        )
                    synced_text = data.get("syncedLyrics") or ""
                    if synced_text.strip():
                        entries, is_word = parse_lrc_entries(synced_text)
                        if entries:
                            status = "word_synced" if is_word else "line_synced"
                            return LyricsResult(status, entries), True

            clean_title, clean_artist = queries[-1]
            search_attempts: list[dict[str, Any]] = []
            if clean_title and clean_artist:
                search_attempts.append(
                    {"track_name": clean_title, "artist_name": clean_artist}
                )
            if clean_title:
                search_attempts.append({"q": f"{clean_artist} {clean_title}".strip()})

            for params in search_attempts:
                resp = requests.get(
                    f"{LRCLIB_BASE_URL}/search",
                    params=params,
                    headers=HTTP_HEADERS,
                    timeout=HTTP_TIMEOUT_SEC,
                )
                if resp.status_code != 200:
                    continue
                items = resp.json()
                if not isinstance(items, list):
                    continue
                best = self._pick_lrclib_match(items, duration_sec)
                if best and best.get("syncedLyrics"):
                    entries, is_word = parse_lrc_entries(best["syncedLyrics"])
                    if entries:
                        status = "word_synced" if is_word else "line_synced"
                        return LyricsResult(status, entries), True
                if any(it.get("instrumental") for it in items if isinstance(it, dict)):
                    return (
                        LyricsResult(
                            "instrumental",
                            [LyricEntry(0, 600_000, "Instrumental", [])],
                        ),
                        True,
                    )
                if any(it.get("plainLyrics") for it in items if isinstance(it, dict)):
                    return (
                        LyricsResult(
                            "unsynced",
                            [LyricEntry(0, 600_000, "No synced lyrics", [])],
                        ),
                        True,
                    )

            return (
                LyricsResult(
                    "not_found", [LyricEntry(0, 600_000, "No synced lyrics", [])]
                ),
                True,
            )
        except requests.RequestException:
            return (
                LyricsResult(
                    "not_found", [LyricEntry(0, 600_000, "Lyrics offline", [])]
                ),
                False,
            )

    def _query_all_providers(
        self, title: str, artist: str, duration_ms: int
    ) -> tuple[LyricsResult, bool]:
        duration_sec = round(duration_ms / 1000) if duration_ms > 0 else 0
        clean_title, clean_artist = clean_track_query(title, artist)
        queries = [(title.strip(), artist.strip())]
        if (clean_title, clean_artist) != queries[0]:
            queries.append((clean_title, clean_artist))

        line_fallback: LyricsResult | None = None
        net_err = False

        bl_res, bl_err = self._query_betterlyrics(queries, duration_sec)
        net_err = net_err or bl_err
        if bl_res is not None:
            if bl_res.status == "word_synced":
                return bl_res, True
            line_fallback = bl_res

        lp_res, lp_err = self._query_lyricsplus(queries, duration_sec)
        net_err = net_err or lp_err
        if lp_res is not None:
            if lp_res.status == "word_synced":
                return lp_res, True
            if line_fallback is None:
                line_fallback = lp_res

        if line_fallback is not None:
            return line_fallback, True

        lrc_res, lrc_cacheable = self._query_lrclib(queries, duration_sec)
        return lrc_res, (lrc_cacheable and not net_err)

    def get_lyrics_sync(
        self, title: str, artist: str, duration_ms: int
    ) -> LyricsResult:
        """Resolve lyrics synchronously (intended for asyncio.to_thread)."""
        local_file = self._find_local_lrc(title, artist)
        if local_file is not None:
            entries, is_word = parse_lrc_entries(
                local_file.read_text(encoding="utf-8", errors="replace")
            )
            if entries:
                return LyricsResult(
                    "word_synced" if is_word else "line_synced", entries
                )

        key = self._cache_key(title, artist, duration_ms)
        if key in self._memory_cache:
            return self._memory_cache[key]

        cached = self._read_disk_cache(key)
        if cached is not None:
            self._memory_cache[key] = cached
            return cached

        result, should_cache = self._query_all_providers(title, artist, duration_ms)
        if should_cache:
            self._memory_cache[key] = result
            self._write_disk_cache(key, result)
        return result


def _timedelta_to_ms(td: Any) -> int:
    if td is None:
        return 0
    try:
        return max(0, int(td.total_seconds() * 1000))
    except Exception:
        return 0


def _pick_session(sessions: list[Any], current: Any, bitchord_only: bool) -> Any:
    """Select the best SMTC session, prioritizing BitChord Desktop."""
    if not sessions:
        return None if bitchord_only else current

    bitchord_sessions = [
        s
        for s in sessions
        if "bitchord" in (getattr(s, "source_app_user_model_id", "") or "").lower()
    ]
    if bitchord_sessions:
        for s in bitchord_sessions:
            try:
                if s.get_playback_info().playback_status == PlaybackStatus.PLAYING:
                    return s
            except Exception:
                pass
        return bitchord_sessions[0]

    if bitchord_only:
        return None

    for s in sessions:
        try:
            if s.get_playback_info().playback_status == PlaybackStatus.PLAYING:
                return s
        except Exception:
            pass
    return current or sessions[0]


async def read_smtc_state(
    manager: Any | None, bitchord_only: bool = False
) -> tuple[Any | None, MediaState | None]:
    """Read current media metadata and timeline from Windows SMTC."""
    if SessionManager is None:
        raise RuntimeError(
            f"winrt-Windows.Media.Control unavailable: {WINRT_IMPORT_ERROR}"
        )

    if manager is None:
        manager = await SessionManager.request_async()

    sessions = list(manager.get_sessions())
    current = manager.get_current_session()
    session = _pick_session(sessions, current, bitchord_only)
    if session is None:
        return manager, None

    playback_info = session.get_playback_info()
    status = playback_info.playback_status
    if status not in (PlaybackStatus.PLAYING, PlaybackStatus.PAUSED):
        return manager, None

    props = await session.try_get_media_properties_async()
    title = (getattr(props, "title", "") or "").strip()
    artist = (getattr(props, "artist", "") or "").strip()
    if not title:
        return manager, None

    timeline = session.get_timeline_properties()
    raw_pos_ms = _timedelta_to_ms(getattr(timeline, "position", None))
    duration_ms = _timedelta_to_ms(getattr(timeline, "end_time", None))
    last_updated = getattr(timeline, "last_updated_time", None)

    return manager, MediaState(
        app_id=getattr(session, "source_app_user_model_id", "") or "",
        title=title,
        artist=artist,
        raw_pos_ms=raw_pos_ms,
        duration_ms=duration_ms,
        is_playing=(status == PlaybackStatus.PLAYING),
        last_updated_utc=last_updated,
    )


def detect_esp32_port(requested: str = "auto") -> str | None:
    """Resolve COM port automatically, preferring COM6 or USB-UART adapters."""
    if requested and requested.lower() != "auto":
        return requested

    ports = list(list_ports.comports())
    for p in ports:
        if p.device.upper() == "COM6":
            return p.device
    for p in ports:
        desc = f"{p.description} {p.manufacturer or ''}".upper()
        if any(tok in desc for tok in ("CP210", "CH340", "SILICON LABS", "UART", "USB")):
            return p.device
    return ports[0].device if ports else None


class SerialLink:
    """Manages non-blocking USB Serial connection and auto-reconnect to ESP32."""

    def __init__(self, port_spec: str, baud: int, dry_run: bool = False) -> None:
        self.port_spec = port_spec
        self.baud = baud
        self.dry_run = dry_run
        self._ser: serial.Serial | None = None
        self._last_connect_attempt = 0.0
        self.active_port_name = "DRY-RUN" if dry_run else ""

    def ensure_connected(self) -> bool:
        if self.dry_run:
            return True
        if self._ser is not None and self._ser.is_open:
            self._drain_rx()
            return True

        now = time.monotonic()
        if now - self._last_connect_attempt < 2.0:
            return False
        self._last_connect_attempt = now

        port = detect_esp32_port(self.port_spec)
        if not port:
            return False

        try:
            ser = serial.Serial(port, self.baud, timeout=0.05, write_timeout=0.5)
            time.sleep(ESP32_BOOT_DELAY_SEC)
            self._ser = ser
            self.active_port_name = port
            self._drain_rx()
            print(f"[Serial] Terhubung ke ESP32 di {port} ({self.baud} baud)")
            return True
        except serial.SerialException as exc:
            print(f"[Serial] Menunggu port {port}: {exc}")
            self._ser = None
            return False

    def _drain_rx(self) -> None:
        if self._ser is None or not self._ser.is_open:
            return
        try:
            while self._ser.in_waiting > 0:
                self._ser.readline()
        except serial.SerialException:
            self.close(send_bye=False)

    def send_line(self, line: str) -> bool:
        if self.dry_run:
            return True
        if not self.ensure_connected() or self._ser is None:
            return False
        try:
            payload = (line.rstrip("\r\n") + "\n").encode("ascii", errors="ignore")
            self._ser.write(payload)
            return True
        except serial.SerialException as exc:
            print(f"[Serial] Koneksi terputus ({exc}), mencoba reconnect...")
            self.close(send_bye=False)
            return False

    def close(self, send_bye: bool = True) -> None:
        if self._ser is None:
            return
        try:
            if send_bye and self._ser.is_open:
                self._ser.write(b"B|OFFLINE\n")
                self._ser.flush()
                time.sleep(0.05)
        except serial.SerialException:
            pass
        try:
            self._ser.close()
        except serial.SerialException:
            pass
        self._ser = None


def _format_mmss(ms: int) -> str:
    sec = min(max(0, ms) // 1000, 99 * 60 + 59)
    return f"{sec // 60:02d}:{sec % 60:02d}"


def _active_word_preview(entry: LyricEntry | None, pos_ms: int, offset_ms: int) -> str:
    """Format terminal dry-run preview highlighting the currently sung syllable/word."""
    if entry is None or not entry.text:
        return "<instrumental>"
    if not entry.segments:
        return entry.text
    rel_ms = max(0, (pos_ms + offset_ms) - entry.start_ms)
    prev_c = 0
    for seg in entry.segments:
        if rel_ms < seg.end_ms:
            if rel_ms < seg.start_ms:
                return entry.text
            return (
                f"{entry.text[:prev_c]}[{entry.text[prev_c:seg.char_end]}]"
                f"{entry.text[seg.char_end:]}"
            )
        prev_c = seg.char_end
    return entry.text


async def bridge_loop(args: argparse.Namespace) -> int:
    provider = LyricsProvider()
    link = SerialLink(args.port, args.baud, dry_run=args.dry_run)
    manager: Any | None = None
    clock = PlaybackClock()

    current_track_id: tuple[str, str] | None = None
    lyrics_result = LyricsResult("not_found", [])
    lyrics_task: asyncio.Task[LyricsResult] | None = None
    loading_track_id: tuple[str, str] | None = None
    none_since_mono: float | None = None

    last_sent_header = ""
    last_sent_packet: str | None = None
    last_sent_playing: bool | None = None
    last_track_refresh = 0.0
    last_progress_tx = 0.0
    last_standby_tx = 0.0
    last_dry_print = ""

    if not args.dry_run:
        link.ensure_connected()

    try:
        while True:
            manager, state = await read_smtc_state(
                manager, bitchord_only=args.bitchord_only
            )
            now_mono = time.monotonic()

            if state is None:
                if none_since_mono is None:
                    none_since_mono = now_mono

                if args.once or (now_mono - none_since_mono) >= NONE_DEBOUNCE_SEC:
                    if lyrics_task is not None and not lyrics_task.done():
                        lyrics_task.cancel()
                    current_track_id = None
                    clock.reset()
                    last_sent_header = ""
                    last_sent_packet = None
                    last_sent_playing = None

                    if args.dry_run:
                        msg = "[Standby] Tidak ada sesi BitChord/media yang aktif."
                        if msg != last_dry_print:
                            print(msg)
                            last_dry_print = msg
                    elif now_mono - last_standby_tx >= STANDBY_TX_INTERVAL_SEC:
                        if link.send_line("S|Play music in BitChord"):
                            last_standby_tx = now_mono

                if args.once:
                    return 0
                await asyncio.sleep(POLL_INTERVAL_SEC)
                continue

            none_since_mono = None
            track_id = (state.title.strip().lower(), state.artist.strip().lower())
            if track_id != current_track_id:
                current_track_id = track_id
                lyrics_result = LyricsResult(
                    "loading", [LyricEntry(0, 600_000, "Loading lyrics...", [])]
                )
                if lyrics_task is not None and not lyrics_task.done():
                    lyrics_task.cancel()
                loading_track_id = track_id
                lyrics_task = asyncio.create_task(
                    asyncio.to_thread(
                        provider.get_lyrics_sync,
                        state.title,
                        state.artist,
                        state.duration_ms,
                    )
                )
                last_sent_header = ""
                last_sent_packet = None

            if lyrics_task is not None and lyrics_task.done():
                if loading_track_id == current_track_id:
                    try:
                        lyrics_result = lyrics_task.result()
                    except asyncio.CancelledError:
                        pass
                    except Exception:
                        lyrics_result = LyricsResult(
                            "not_found",
                            [LyricEntry(0, 600_000, "No synced lyrics", [])],
                        )
                    last_sent_packet = None
                lyrics_task = None

            if args.once and lyrics_task is not None:
                try:
                    lyrics_result = await lyrics_task
                except Exception:
                    lyrics_result = LyricsResult(
                        "not_found",
                        [LyricEntry(0, 600_000, "No synced lyrics", [])],
                    )
                lyrics_task = None

            effective_dur_ms = state.duration_ms
            if effective_dur_ms <= 0 and lyrics_result.status in (
                "word_synced",
                "line_synced",
            ):
                non_empty = [e for e in lyrics_result.entries if e.text]
                if non_empty:
                    effective_dur_ms = max(e.end_ms for e in non_empty)

            live_pos_ms = clock.update(
                track_id=track_id,
                raw_pos_ms=state.raw_pos_ms,
                duration_ms=effective_dur_ms,
                is_playing=state.is_playing,
                last_updated_utc=state.last_updated_utc,
                now_utc=datetime.now(timezone.utc),
                now_mono=now_mono,
            )

            header_raw = (
                f"{state.title} - {state.artist}" if state.artist else state.title
            )
            header_text = sanitize_oled_text(header_raw, max_len=110)

            if lyrics_result.status in ("word_synced", "line_synced"):
                active_entry = find_active_entry(
                    lyrics_result.entries, live_pos_ms, offset_ms=args.offset_ms
                )
            elif lyrics_result.entries:
                active_entry = lyrics_result.entries[0]
            else:
                active_entry = None

            lyric_packet = (
                format_word_packet(active_entry, offset_ms=args.offset_ms)
                if active_entry is not None
                else "L|"
            )

            if args.dry_run:
                play_flag = ">" if state.is_playing else "||"
                preview_lyric = _active_word_preview(
                    active_entry, live_pos_ms, args.offset_ms
                )
                preview = (
                    f"[{state.app_id or 'SMTC'}] {play_flag} {header_text} | "
                    f"{_format_mmss(live_pos_ms)}/{_format_mmss(effective_dur_ms)} | "
                    f"Lyric({lyrics_result.status}): {preview_lyric}"
                )
                if preview != last_dry_print:
                    print(preview)
                    last_dry_print = preview
            else:
                force_refresh = (now_mono - last_track_refresh) >= TRACK_REFRESH_INTERVAL_SEC
                sent_any = False
                if header_text != last_sent_header or force_refresh:
                    if link.send_line(f"T|{header_text}"):
                        last_sent_header = header_text
                        sent_any = True
                if lyric_packet != last_sent_packet or force_refresh:
                    if link.send_line(lyric_packet):
                        last_sent_packet = lyric_packet
                        sent_any = True
                if force_refresh and sent_any:
                    last_track_refresh = now_mono

                if (
                    (now_mono - last_progress_tx) >= PROGRESS_TX_INTERVAL_SEC
                    or sent_any
                    or state.is_playing != last_sent_playing
                ):
                    play_int = 1 if state.is_playing else 0
                    if link.send_line(
                        f"P|{live_pos_ms}|{effective_dur_ms}|{play_int}"
                    ):
                        last_progress_tx = now_mono
                        last_sent_playing = state.is_playing

            if args.once:
                return 0
            await asyncio.sleep(POLL_INTERVAL_SEC)
    finally:
        if not args.dry_run:
            link.close(send_bye=True)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Sinkronisasi lirik kata-per-kata BitChord (Windows SMTC) ke ESP32 OLED via USB Serial."
    )
    parser.add_argument(
        "--port",
        default="auto",
        help="Port serial ESP32 (default: auto, prioritas COM6).",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=DEFAULT_BAUD,
        help=f"Baud rate serial (default: {DEFAULT_BAUD}).",
    )
    parser.add_argument(
        "--bitchord-only",
        action="store_true",
        help="Hanya baca sesi SMTC dari BitChord Desktop (abaikan browser/app lain).",
    )
    parser.add_argument(
        "--offset-ms",
        type=int,
        default=0,
        help="Geser timing lirik dalam milidetik (+ lebih cepat, - lebih lambat).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Cetak output lirik di terminal tanpa membuka port serial.",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="Jalankan satu siklus pembacaan lalu keluar (untuk pengujian).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    try:
        return asyncio.run(bridge_loop(args))
    except KeyboardInterrupt:
        print("\n[Bridge] Dihentikan oleh pengguna. ESP32 kembali ke mode Offline.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
