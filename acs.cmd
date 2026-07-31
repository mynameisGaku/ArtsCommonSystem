@echo off
setlocal DisableDelayedExpansion

set "ACS_OPERATIONS=%~dp0acs\scripts\project_operations.ps1"
set "ACS_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%ACS_OPERATIONS%" (
    >&2 echo [acs] operation script was not found: "%ACS_OPERATIONS%"
    exit /b 3
)

if not exist "%ACS_POWERSHELL%" (
    >&2 echo [acs] Windows PowerShell was not found: "%ACS_POWERSHELL%"
    exit /b 3
)

set "ACS_PROJECT_OPERATIONS_ARGUMENT_COUNT=0"

:acs_collect_arguments
if "%~1"=="" goto acs_launch
set "ACS_PROJECT_OPERATIONS_ARGUMENT_%ACS_PROJECT_OPERATIONS_ARGUMENT_COUNT%=%~1"
set /a ACS_PROJECT_OPERATIONS_ARGUMENT_COUNT+=1 >nul
shift
goto acs_collect_arguments

:acs_launch
"%ACS_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ACS_OPERATIONS%" __ACS_CMD_ENVIRONMENT_ARGUMENTS__
set "ACS_EXIT_CODE=%ERRORLEVEL%"
exit /b %ACS_EXIT_CODE%
