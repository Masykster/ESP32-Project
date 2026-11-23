"""Unit tests for lyric_bridge parsing, TTML/LyricsPlus word timing, and serialization."""

from datetime import datetime, timedelta, timezone

from lyric_bridge import (
    LyricEntry,
    PlaybackClock,
    TimedSegment,
    build_proportional_segments,
    clean_track_query,
    compute_live_position_ms,
    find_active_entry,
    find_active_lyric,
    format_word_packet,
    parse_lrc,
    parse_lrc_entries,
    parse_lyricsplus_json,
    parse_ttml,
    parse_ttml_time_ms,
    sanitize_oled_text,
)


def test_parse_lrc_standard_multi_timestamp_and_karaoke_tags() -> None:
    sample = """
    [ti:Midnight Echoes]
    [ar:BitChord Artist]
    [offset:500]
    [00:05.50]First line
    [00:10.125]<00:10.12>Word <00:10.50>by word
    [00:20.00][00:40.00]Repeated chorus
    [00:30.00]
    """
    parsed = parse_lrc(sample)
    assert parsed == [
        (5000, "First line"),
        (9625, "Word by word"),
        (19500, "Repeated chorus"),
        (29500, ""),
        (39500, "Repeated chorus"),
    ]


def test_parse_lrc_entries_enhanced_and_standard() -> None:
    enhanced = "[00:10.00]<00:10.00>Word <00:10.50>by <00:10.90>word\n[00:12.00]Next line"
    entries, is_word = parse_lrc_entries(enhanced)
    assert is_word is True
    assert entries[0].start_ms == 0  # intro instrumental marker (< 2500ms skipped? 10000 >= 2500)
    assert entries[1].text == "Word by word"
    assert [(s.start_ms, s.end_ms, s.char_end) for s in entries[1].segments] == [
        (0, 500, 5),
        (500, 900, 8),
        (900, 1500, 12),
    ]
    assert entries[2].text == "Next line"
    assert entries[2].segments[-1].char_end == len("Next line")


def test_parse_ttml_time_formats() -> None:
    assert parse_ttml_time_ms("12.819") == 12819
    assert parse_ttml_time_ms("1:46.299") == 106299
    assert parse_ttml_time_ms("00:00:00.160") == 160
    assert parse_ttml_time_ms("12.5s") == 12500
    assert parse_ttml_time_ms("450ms") == 450


def test_parse_ttml_word_syllables_and_x_bg_skip() -> None:
    ttml = """
    <tt xmlns="http://www.w3.org/ns/ttml"
        xmlns:itunes="http://music.apple.com/lyric-ttml-internal"
        xmlns:ttm="http://www.w3.org/ns/ttml#metadata"
        itunes:timing="Word" xml:lang="id">
      <body>
        <div>
          <p begin="15.050" end="17.898"><span begin="15.050" end="15.478">Ma</span><span begin="15.478" end="15.715">sih</span><span begin="15.758" end="17.898">kah</span></p>
          <p begin="18.918" end="21.301"><span begin="18.918" end="19.384">Lu</span><span begin="19.384" end="19.864">ka</span> <span begin="19.864" end="20.149">i</span><span begin="20.149" end="21.301">tu</span> <span ttm:role="x-bg"><span begin="20.000" end="21.500">(echo)</span></span></p>
        </div>
      </body>
    </tt>
    """
    entries, is_word = parse_ttml(ttml)
    assert is_word is True
    # Entry 0 is intro instrumental (0..15050), Entry 1 is "Masihkah", Entry 2 is "Luka itu"
    assert entries[0].text == ""
    assert entries[1].text == "Masihkah"
    assert entries[1].start_ms == 15050
    assert [(s.start_ms, s.end_ms, s.char_end) for s in entries[1].segments] == [
        (0, 428, 2),
        (428, 665, 5),
        (708, 2848, 8),
    ]
    assert entries[2].text == "Luka itu"
    assert [entries[2].text[:s.char_end] for s in entries[2].segments] == [
        "Lu",
        "Luka ",
        "Luka i",
        "Luka itu",
    ]


def test_parse_lyricsplus_json_syllables() -> None:
    payload = {
        "type": "Word",
        "lyrics": [
            {
                "time": 12819,
                "duration": 1767,
                "text": "Hai",
                "syllabus": [{"time": 12819, "duration": 1767, "text": "Hai"}],
            },
            {
                "time": 15050,
                "duration": 2848,
                "text": "Luka itu",
                "syllabus": [
                    {"time": 15050, "duration": 400, "text": "Luka"},
                    {"time": 15500, "duration": 600, "text": "itu"},
                ],
            },
        ],
    }
    entries, is_word = parse_lyricsplus_json(payload)
    assert is_word is True
    sung = [e for e in entries if e.text]
    assert sung[0].text == "Hai"
    assert sung[1].text == "Luka itu"
    assert sung[1].segments[0].char_end == 5  # "Luka "
    assert sung[1].segments[1].char_end == 8  # "Luka itu"


def test_format_word_packet_and_coalesce() -> None:
    entry = LyricEntry(
        15050,
        17898,
        "Masihkah",
        [TimedSegment(0, 428, 2), TimedSegment(428, 665, 5), TimedSegment(708, 2848, 8)],
    )
    pkt = format_word_packet(entry)
    assert pkt == "W|15050|17898|Masihkah|0:428:2,428:665:5,708:2848:8"

    many_segs = [TimedSegment(i * 50, i * 50 + 40, i + 1) for i in range(60)]
    long_entry = LyricEntry(1000, 4000, "A" * 60, many_segs)
    compact_pkt = format_word_packet(long_entry, max_packet_len=240)
    assert compact_pkt.startswith("W|1000|4000|")
    assert len(compact_pkt) <= 240
    assert compact_pkt.endswith(":60")


def test_find_active_entry_and_proportional_segments() -> None:
    segs = build_proportional_segments("One two three", 1500)
    assert len(segs) == 3
    assert segs[-1].end_ms == 1500
    assert segs[-1].char_end == len("One two three")

    entries = [
        LyricEntry(0, 4000, "", []),
        LyricEntry(4000, 8000, "Line one", build_proportional_segments("Line one", 4000)),
        LyricEntry(8500, 12000, "Line two", build_proportional_segments("Line two", 3500)),
    ]
    assert find_active_entry(entries, 2000).text == ""
    assert find_active_entry(entries, 4000).text == "Line one"
    assert find_active_entry(entries, 8500).text == "Line two"
    assert find_active_entry(entries, 3700, offset_ms=400).text == "Line one"


def test_find_active_lyric_with_and_without_offset() -> None:
    lines = [(0, ""), (4000, "Line one"), (8500, "Line two"), (12000, "")]
    assert find_active_lyric(lines, 1500) == ""
    assert find_active_lyric(lines, 4000) == "Line one"
    assert find_active_lyric(lines, 8499) == "Line one"
    assert find_active_lyric(lines, 8500) == "Line two"
    assert find_active_lyric(lines, 15000) == ""
    assert find_active_lyric(lines, 3700, offset_ms=400) == "Line one"
    assert find_active_lyric(lines, 4200, offset_ms=-300) == ""


def test_sanitize_oled_text_normalizes_unicode_and_protocol_pipes() -> None:
    raw = "It’s “Café” — part 1|2 <00:12.34> …\t\n"
    assert sanitize_oled_text(raw) == 'It\'s "Cafe" - part 1/2 ...'

    long_text = "A" * 50
    clamped = sanitize_oled_text(long_text, max_len=20)
    assert len(clamped) == 20
    assert clamped.endswith("...")


def test_clean_track_query_strips_video_and_topic_noise() -> None:
    title, artist = clean_track_query(
        "Night Dancer (Official Music Video) [Lyrics] feat. Guest",
        "imase - Topic",
    )
    assert title == "Night Dancer"
    assert artist == "imase"


def test_compute_live_position_ms_interpolates_playing_and_freezes_paused() -> None:
    now = datetime(2026, 11, 21, 12, 0, 10, tzinfo=timezone.utc)
    last_updated = now - timedelta(seconds=3, milliseconds=250)

    playing_pos = compute_live_position_ms(
        raw_pos_ms=10_000,
        duration_ms=180_000,
        is_playing=True,
        last_updated_utc=last_updated,
        now_utc=now,
    )
    assert playing_pos == 13_250

    paused_pos = compute_live_position_ms(
        raw_pos_ms=10_000,
        duration_ms=180_000,
        is_playing=False,
        last_updated_utc=last_updated,
        now_utc=now,
    )
    assert paused_pos == 10_000


def test_compute_live_position_ms_uses_fallback_when_smtc_timestamp_invalid() -> None:
    now = datetime(2026, 11, 21, 12, 0, 10, tzinfo=timezone.utc)
    invalid_dt = datetime(1601, 1, 1, 0, 0, 0, tzinfo=timezone.utc)

    pos = compute_live_position_ms(
        raw_pos_ms=5_000,
        duration_ms=60_000,
        is_playing=True,
        last_updated_utc=invalid_dt,
        now_utc=now,
        fallback_elapsed_ms=2_400,
    )
    assert pos == 7_400


def test_playback_clock_pause_and_resume_with_bitchord_zero_pos() -> None:
    clock = PlaybackClock()
    t0 = datetime(2026, 11, 21, 13, 57, 0, tzinfo=timezone.utc)
    track = ("all the small things", "blink-182")

    assert clock.update(track, 0, 168_000, True, t0, t0, now_mono=100.0) == 0
    assert clock.update(track, 0, 168_000, True, t0, t0 + timedelta(seconds=67), now_mono=167.0) == 67_000

    # BitChord reports raw_pos=0 when paused
    paused_at = clock.update(
        track,
        0,
        168_000,
        False,
        t0 + timedelta(seconds=67),
        t0 + timedelta(seconds=67, milliseconds=200),
        now_mono=167.2,
    )
    assert paused_at == 67_200

    # Remains frozen across subsequent paused polls even with raw_pos=0
    assert (
        clock.update(
            track,
            0,
            168_000,
            False,
            t0 + timedelta(seconds=67),
            t0 + timedelta(seconds=75),
            now_mono=175.0,
        )
        == 67_200
    )

    # Resume continues smoothly from frozen position
    assert (
        clock.update(
            track,
            0,
            168_000,
            True,
            t0 + timedelta(seconds=77),
            t0 + timedelta(seconds=77),
            now_mono=177.0,
        )
        == 67_200
    )
    assert (
        clock.update(
            track,
            0,
            168_000,
            True,
            t0 + timedelta(seconds=77),
            t0 + timedelta(seconds=78),
            now_mono=178.0,
        )
        == 68_200
    )


def test_playback_clock_next_song_resets_despite_stale_last_updated() -> None:
    clock = PlaybackClock()
    old_upd = datetime(2026, 11, 21, 13, 50, 0, tzinfo=timezone.utc)
    now = old_upd + timedelta(seconds=242)

    clock.update(("i miss you", "blink-182"), 0, 227_000, True, old_upd, now, now_mono=100.0)
    # Next song arrives with stale last_updated_utc from previous track
    next_pos = clock.update(
        ("all the small things", "blink-182"),
        0,
        168_000,
        True,
        old_upd,
        now + timedelta(milliseconds=200),
        now_mono=100.2,
    )
    assert next_pos == 0


def test_playback_clock_duration_settling_does_not_reset() -> None:
    clock = PlaybackClock()
    t0 = datetime(2026, 11, 21, 13, 57, 0, tzinfo=timezone.utc)
    track = ("bunga maaf", "the lantis")

    assert clock.update(track, 0, 0, True, t0, t0, now_mono=10.0) == 0
    assert clock.update(track, 0, 253_000, True, t0, t0 + timedelta(seconds=5), now_mono=15.0) == 5_000


def test_playback_clock_seek_updates_immediately() -> None:
    clock = PlaybackClock()
    t0 = datetime(2026, 11, 21, 13, 57, 0, tzinfo=timezone.utc)
    track = ("bunga maaf", "the lantis")

    assert clock.update(track, 0, 253_000, True, t0, t0, now_mono=10.0) == 0
    assert clock.update(track, 0, 253_000, True, t0, t0 + timedelta(seconds=10), now_mono=20.0) == 10_000
    # User seeks forward while playing
    assert (
        clock.update(
            track,
            95_000,
            253_000,
            True,
            t0 + timedelta(seconds=11),
            t0 + timedelta(seconds=11),
            now_mono=21.0,
        )
        == 95_000
    )
    # User seeks while paused
    assert (
        clock.update(
            track,
            40_000,
            253_000,
            False,
            t0 + timedelta(seconds=12),
            t0 + timedelta(seconds=12),
            now_mono=22.0,
        )
        == 40_000
    )


def test_playback_clock_mid_song_initial_startup() -> None:
    clock = PlaybackClock()
    t0 = datetime(2026, 11, 21, 13, 57, 0, tzinfo=timezone.utc)
    track = ("bunga maaf", "the lantis")

    pos = clock.update(
        track,
        0,
        253_000,
        True,
        t0,
        t0 + timedelta(seconds=45),
        now_mono=50.0,
    )
    assert pos == 45_000
