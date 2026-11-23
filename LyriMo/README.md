# LyriMo — ESP32 OLED Word-Sync Lyric Display (BitChord + Offline Demo)

Projek tampilan lirik musik 3-zona pada layar OLED SSD1306 **128×64 piksel** menggunakan **ESP32 + U8g2**.

Mendukung dua mode kerja secara otomatis (**Hybrid**) dengan **animasi kata-per-kata & transisi huruf-per-huruf** seperti BitChord:
1. **Live Sync BitChord via USB Serial**: Script Python ringan di laptop Windows membaca lagu dan detik pemutaran aktif langsung dari **BitChord Desktop** (lewat Windows SMTC), mengambil lirik silabel/per-kata (prioritas: `lyrics_local/*.lrc` → **BetterLyrics TTML** → **LyricsPlus** → **LRCLIB**), lalu mengirimkannya ke ESP32 lewat kabel USB (`COM6`).
2. **Standalone Offline Demo (Fallback Otomatis)**: Jika script PC tidak berjalan atau ESP32 hanya dicolok ke charger/powerbank, setelah 6 detik (atau saat script ditutup dengan `Ctrl+C`), ESP32 otomatis memutar lagu demo tertanam dari [`demo_lyrics.h`](demo_lyrics.h) menggunakan timer internal `millis()` lengkap dengan animasi kata-per-kata.

---

## Tata Letak Layar 128×64 (3 Zona)

```text
y=0..11   (12 px)  [>] Judul Lagu - Nama Artis (auto-scroll)   <- Header + garis pemisah
y=12..53  (42 px)       Baris lirik aktif (kata & huruf         <- Karaoke dither->bright +
                        menyala terang bertahap + slide-up)        garis progres tipis 1 px
y=54..63  (10 px)  01:23 [============------] 03:45            <- Progress bar + mm:ss
```

- **Header (12 px)**: Ikon Play/Pause + `Judul - Artis` (`u8g2_font_6x10_tf`). Teks panjang otomatis bergulir mendatar (marquee) setelah jeda 1,5 detik.
- **Main Lyric Area (42 px)**: Auto word-wrap rata tengah (`u8g2_font_helvB10_tr`, otomatis mengecil ke `u8g2_font_helvB08_tr` bila baris panjang). Kata yang belum dinyanyikan tampil redup, lalu menyala penuh **piksel-demi-piksel & huruf-demi-huruf** sesuai timing silabel, disertai garis progres 1 px dan transisi *slide-up* ~160 ms antar baris. Jeda instrumental menampilkan ikon not musik pixel.
- **Progress Bar (10 px)**: Indikator waktu berjalan & durasi (`mm:ss`) + progress bar 72 px dengan interpolasi halus di ESP32.

---

## Wiring & Pengaturan Board

```text
OLED VCC -> 3V3
OLED GND -> GND
OLED SDA -> GPIO21
OLED SCL -> GPIO22
```

| Setting | Nilai |
|---|---|
| Board | ESP32 Dev Module |
| Port | COM6 |
| Baud Rate | 115200 |
| Library | U8g2 (`2.37.1`) |

> Jika OLED tetap blank, coba alamat I2C `0x3D` dengan membuka komentar `u8g2.setI2CAddress(0x3D * 2);` di dalam `setup()` pada [`LyriMo.ino`](LyriMo.ino).

---

## Menjalankan Live Sync dengan BitChord Desktop

1. Install dependensi Python (cukup sekali):
   ```powershell
   cd C:\Users\HP\Documents\Arduino\ESP32-Project\LyriMo\pc_bridge
   python -m pip install -r requirements.txt
   ```
2. Pastikan Serial Monitor Arduino IDE sedang **tertutup** (agar port `COM6` tidak bentrok).
3. Putar lagu di **BitChord Desktop**, lalu jalankan [`pc_bridge/run_bridge.bat`](pc_bridge/run_bridge.bat) (klik ganda atau lewat terminal).
4. Tekan `Ctrl+C` di jendela terminal untuk menghentikan bridge dan mengembalikan ESP32 ke mode Offline Demo seketika.

### Opsi Tambahan `lyric_bridge.py`

```powershell
python lyric_bridge.py --port COM6          # Tentukan port serial manual
python lyric_bridge.py --bitchord-only      # Abaikan media player lain di Windows
python lyric_bridge.py --offset-ms 400      # Majukan lirik 400 ms (+ cepat, - lambat)
python lyric_bridge.py --dry-run            # Uji di terminal tanpa ESP32
```

---

## Kustomisasi Lagu Offline & File `.lrc` Lokal

- **Mengganti lagu Offline Demo**: Edit `DEMO_HEADER`, `DEMO_DURATION_MS`, dan array `DEMO_LYRICS` di [`demo_lyrics.h`](demo_lyrics.h), lalu upload ulang sketch ke ESP32.
- **Menambah lirik `.lrc` manual untuk Mode Live**: Simpan file `.lrc` (atau Enhanced LRC `<mm:ss.xx>`) di folder `pc_bridge/lyrics_local/` dengan nama `Nama Artis - Judul Lagu.lrc` atau `Judul Lagu.lrc`. Bridge memprioritaskan file lokal sebelum **BetterLyrics**, **LyricsPlus**, dan **LRCLIB** (hasil fetch disimpan ke `pc_bridge/.lrc_cache/*_v2.json`).
- **Backup kode demo lama**: Tersimpan utuh di `backup/ESP32_OLED_U8g2_original.ino.bak`.

---

## Troubleshooting

- **Gagal upload / `Access is denied` di `COM6`**: Tutup tab **Serial Monitor** di Arduino IDE (klik `×` pada panel bawah Serial Monitor) atau hentikan `lyric_bridge.py`.
- **Lirik agak telat atau terlalu cepat**: Gunakan parameter `--offset-ms` (misalnya `python lyric_bridge.py --offset-ms 500` atau `-500`).
- **Karakter Non-Latin (Jepang/Korea/dll.)**: Font bawaan U8g2 yang dipakai adalah Latin ASCII; lirik non-Latin penuh akan ditampilkan sebagai `(Non-Latin lyric)`, sedangkan huruf beraksen otomatis dikonversi ke ASCII.
- **Tidak ada lirik tersinkron**: Beberapa lagu baru atau instrumental hanya memiliki lirik biasa sehingga OLED menampilkan `No synced lyrics` atau `Instrumental` (bisa diatasi dengan menaruh file `.lrc` sendiri di `pc_bridge/lyrics_local/`).
