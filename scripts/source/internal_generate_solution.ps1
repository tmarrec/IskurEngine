# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

param(
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

function Get-WindowsSdkBinX64Path {
    $candidates = [System.Collections.Generic.List[string]]::new()

    if ($env:WindowsSdkDir) {
        if ($env:WindowsSDKVersion) {
            $sdkVersion = $env:WindowsSDKVersion.TrimEnd('\')
            $candidates.Add((Join-Path $env:WindowsSdkDir "bin\$sdkVersion\x64"))
        }
        $candidates.Add((Join-Path $env:WindowsSdkDir "bin\x64"))
    }

    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (-not $programFilesX86) {
        $programFilesX86 = "C:\Program Files (x86)"
    }
    $sdkBinRoot = Join-Path $programFilesX86 "Windows Kits\10\bin"
    if (Test-Path $sdkBinRoot) {
        $versionDirs = Get-ChildItem -Path $sdkBinRoot -Directory |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending
        foreach ($dir in $versionDirs) {
            $candidates.Add((Join-Path $dir.FullName "x64"))
        }
        $candidates.Add((Join-Path $sdkBinRoot "x64"))
    }

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Path (Join-Path $candidate "dxil.dll")) {
            return $candidate
        }
    }

    return $null
}

$winKitsPath = Get-WindowsSdkBinX64Path
if (-not $winKitsPath) {
    Fail "Windows SDK not found (dxil.dll missing)." 1
}
$dxilSource = Join-Path $winKitsPath 'dxil.dll'

$root = Get-RepoRoot

cmake -S $root --preset=default
if ($LASTEXITCODE -ne 0) {
    Fail "CMake failed." $LASTEXITCODE
}

cmake -S (Join-Path $root 'code\tools\IskurScenePacker') --preset=default
if ($LASTEXITCODE -ne 0) {
    Fail "CMake failed (packer)." $LASTEXITCODE
}

foreach ($dir in @('build\engine', 'build\packer', 'bin', 'bin\Debug', 'bin\Release')) {
    $path = Join-Path $root $dir
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory -Path $path | Out-Null
    }
}

$links = @(
    @{Link=('build','engine','data'); Target=('data')},
    @{Link=('build','packer','data'); Target=('data')},
    @{Link=('bin','data');           Target=('data')},
    @{Link=('bin','Debug','data');   Target=('data')},
    @{Link=('bin','Release','data'); Target=('data')}
)
foreach ($link in $links) {
    $linkPath   = Join-Path $root (Join-Path -Path $link.Link[0] -ChildPath ($link.Link[1..($link.Link.Length-1)] -join '\'))
    $targetPath = Join-Path $root $link.Target
    $parent     = Split-Path -Parent $linkPath
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    if (-not (Test-Path $linkPath)) { New-Item -Path $linkPath -ItemType Junction -Target $targetPath | Out-Null }
}

foreach ($d in @('build\engine', 'bin\Debug', 'bin\Release')) {
    Copy-Item -Path $dxilSource -Destination (Join-Path $root "$d\dxil.dll") -Force
}
