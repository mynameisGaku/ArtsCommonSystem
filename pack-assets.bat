@echo off
rem ACS pack-assets - double-click to convert the assets\ folder into a single
rem .acpak ACS asset archive (drives the acs_assetpack CLI, building it first if
rem needed). Thin wrapper around pack-assets.ps1 (same pattern as generate.bat).
rem   pack-assets.bat                 assets\ -> game.acpak (LZ4)
rem   pack-assets.bat -Encrypt        AES-256-GCM (key in assets.key, auto-generated)
rem   pack-assets.bat -In art -Out art.acpak
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pack-assets.ps1" %*
pause
