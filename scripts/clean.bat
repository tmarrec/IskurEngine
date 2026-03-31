@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0source\clean.ps1" %*
set "EXIT_CODE=%errorlevel%"
if not "%EXIT_CODE%"=="0" (
  echo Clean failed.
  pause
  exit /b %EXIT_CODE%
)

echo Clean completed successfully.
pause
exit /b %EXIT_CODE%
