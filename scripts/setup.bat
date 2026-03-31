@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0source\setup.ps1" %*
set "EXIT_CODE=%errorlevel%"
if not "%EXIT_CODE%"=="0" (
  echo Setup failed.
  pause
  exit /b %EXIT_CODE%
)

echo Setup completed successfully.
pause
exit /b %EXIT_CODE%
