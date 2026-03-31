@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0source\extract_scenes.ps1" %*
set "EXIT_CODE=%errorlevel%"
if not "%EXIT_CODE%"=="0" (
  echo Scene extraction failed.
  pause
  exit /b %EXIT_CODE%
)

echo Scene extraction completed successfully.
pause
exit /b %EXIT_CODE%
