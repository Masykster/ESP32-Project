@echo off
setlocal
cd /d "%~dp0"
echo ============================================================
echo   LyriMo (BitChord -^> ESP32 OLED Lyric Bridge, USB Serial 115200)
echo   Tekan Ctrl+C untuk berhenti dan kembalikan ESP32 ke Offline
echo ============================================================
echo.
python lyric_bridge.py %*
if errorlevel 1 (
    echo.
    echo Bridge berhenti dengan error. Pastikan dependencies sudah terinstall:
    echo   python -m pip install -r requirements.txt
    pause
)
