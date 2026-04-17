@echo off
call "%~dp0source\invoke_script.bat" "%~dp0source\internal_build.ps1" "Release build" -Config Release -NoPause
