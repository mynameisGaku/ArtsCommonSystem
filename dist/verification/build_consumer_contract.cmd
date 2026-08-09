@echo off
REM Build the consumer contract against the single-header ACS distribution.
REM Usage:  build_consumer_contract.cmd [Debug|Release]   (default Debug)
REM advapi32 and comdlg32 are supplied by the acs.h auto-link directives.
setlocal
set CFG=%1
if "%CFG%"=="" set CFG=Debug
set DIST=%~dp0..
if /I "%CFG%"=="Release" ( set CRT=/MD& set OPT=/O2 ) else ( set CRT=/MDd& set OPT=/Od )
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cl /nologo /utf-8 %OPT% /std:c++20 /EHsc /GR- /D_HAS_EXCEPTIONS=1 %CRT% /permissive- /Zc:__cplusplus /Zc:preprocessor ^
   /I "%DIST%" "%~dp0consumer_contract.cpp" /Fe:"%~dp0consumer_contract.exe" /Fo:"%~dp0consumer_contract.obj" ^
   /link /LIBPATH:"%DIST%\lib\x64\%CFG%"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo BUILD OK -^> "%~dp0consumer_contract.exe"
endlocal
