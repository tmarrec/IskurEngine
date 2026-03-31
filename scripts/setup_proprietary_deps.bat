@echo off
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0source\setup_proprietary_deps.ps1" %*
set "EXIT_CODE=%errorlevel%"
if not "%EXIT_CODE%"=="0" (
  echo Proprietary dependency setup failed.
  pause
  exit /b %EXIT_CODE%
)

echo Proprietary dependency setup completed successfully.
pause
exit /b %EXIT_CODE%
