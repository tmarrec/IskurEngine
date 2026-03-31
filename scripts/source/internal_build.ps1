# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [switch]$NoPause
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

function PauseIfNeeded([string]$Message) {
    if (-not $NoPause) {
        Read-Host $Message | Out-Null
    }
}

function Fail([string]$Message, [int]$Code = 1) {
    Write-Host "ERROR: $Message"
    PauseIfNeeded "Press Enter to close..."
    exit $Code
}

$root = Get-RepoRoot
$engineBuildDir = Join-Path $root 'build\engine'
$packerBuildDir = Join-Path $root 'build\packer'

if (-not (Test-Path (Join-Path $engineBuildDir 'CMakeCache.txt'))) {
    Fail "Engine solution not generated. Run scripts\generate_solution.bat first." 1
}
if (-not (Test-Path (Join-Path $packerBuildDir 'CMakeCache.txt'))) {
    Fail "Scene packer solution not generated. Run scripts\generate_solution.bat first." 1
}

cmake --build $engineBuildDir --config $Config --target IskurEngine
if ($LASTEXITCODE -ne 0) {
    Fail "Build failed (engine, $Config)." $LASTEXITCODE
}

cmake --build $packerBuildDir --config $Config --target IskurScenePacker
if ($LASTEXITCODE -ne 0) {
    Fail "Build failed (packer, $Config)." $LASTEXITCODE
}
