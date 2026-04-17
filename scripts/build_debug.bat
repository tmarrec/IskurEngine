@echo off
call "%~dp0source\invoke_script.bat" "%~dp0source\internal_build.ps1" "Debug build" -Config Debug -NoPause
