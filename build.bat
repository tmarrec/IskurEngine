rem Iškur Engine
rem Copyright (c) 2025 Tristan Marrec
rem Licensed under the MIT License.
rem See the LICENSE file in the project root for license information.

@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1"
exit /b