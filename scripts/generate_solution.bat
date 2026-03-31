rem Iskur Engine
rem Copyright (c) 2025 Tristan Marrec
rem Licensed under the MIT License.
rem See the LICENSE file in the project root for license information.

@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0source\internal_generate_solution.ps1" -NoPause
set "EXIT_CODE=%errorlevel%"
if not "%EXIT_CODE%"=="0" (
  echo Solution generation failed.
  pause
  exit /b %EXIT_CODE%
)

echo Solution generation completed successfully.
pause
exit /b %EXIT_CODE%
