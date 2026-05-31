@echo off
rem ACS clean-up - double-click to scan the tree for regenerable junk (build
rem output, intermediate files, IDE caches, scratch), see what was found, and
rem confirm before anything is deleted. Thin wrapper around clean-up.ps1 (same
rem pattern as generate.bat). Pass -y to delete the found set without a prompt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clean-up.ps1" %*
if /i not "%~1"=="-y" pause
