@echo off
rem ACS - double-click to generate the single solution and open it in Visual Studio.
rem Extra options are forwarded to generate.ps1 (e.g. generate.bat -AllBackends).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0generate.ps1" -Open %*
