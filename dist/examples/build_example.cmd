@echo off
REM Build the example consumer against the single-header ACS distribution.
REM Usage:  build_example.cmd [Debug|Release]   (default Debug)
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Debug
set DIST=%~dp0..
if /I "%CFG%"=="Release" ( set CRT=/MD& set OPT=/O2 ) else ( set CRT=/MDd& set OPT=/Od )
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cl /nologo /utf-8 %OPT% /std:c++20 /EHsc /GR- /D_HAS_EXCEPTIONS=1 %CRT% /permissive- /Zc:__cplusplus /Zc:preprocessor ^
   /I "%DIST%" "%~dp0check.cpp" /Fe:"%~dp0check.exe" /Fo:"%~dp0check.obj" ^
   /link /LIBPATH:"%DIST%\lib\x64\%CFG%"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo BUILD OK -^> "%~dp0check.exe"
endlocal
