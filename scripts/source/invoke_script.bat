rem Iskur Engine
rem Copyright (c) 2026 Tristan Marrec
rem Licensed under the MIT License.
rem See the LICENSE file in the project root for license information.

@echo off
setlocal

if "%~1"=="" (
  echo Missing PowerShell script path.
  exit /b 1
)

if "%~2"=="" (
  echo Missing task name.
  exit /b 1
)

set "SCRIPT_PATH=%~f1"
set "TASK_NAME=%~2"
shift
shift

if not exist "%SCRIPT_PATH%" (
  echo PowerShell script not found: "%SCRIPT_PATH%"
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_PATH%" %*
set "EXIT_CODE=%errorlevel%"
if not "%EXIT_CODE%"=="0" (
  echo %TASK_NAME% failed.
  pause
  exit /b %EXIT_CODE%
)

echo %TASK_NAME% completed successfully.
pause
exit /b %EXIT_CODE%
