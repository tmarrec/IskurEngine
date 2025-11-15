# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

$currentUser = New-Object Security.Principal.WindowsPrincipal $([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentUser.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Restarting script as administrator..."
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'powershell.exe'
    $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Definition)`""
    $psi.Verb = 'runas'
    [System.Diagnostics.Process]::Start($psi) | Out-Null
    exit
}

$winKitsPath = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'
if (-not (Test-Path $winKitsPath)) {
    Write-Error "Windows SDK not found."
    exit 1
}
$dxilSource = Join-Path $winKitsPath 'dxil.dll'

$root = Split-Path -Parent $PSScriptRoot

cmake -S $root --preset=default
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake failed."
    exit $LASTEXITCODE
}

foreach ($dir in @('build', 'bin', 'bin\Debug', 'bin\Release')) {
    $path = Join-Path $root $dir
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory -Path $path | Out-Null
    }
}

$links = @(
    @{Link=('build','data');         Target=('data')},
    @{Link=('bin','data');           Target=('data')},
    @{Link=('bin','Debug','data');   Target=('data')},
    @{Link=('bin','Release','data'); Target=('data')}
)
foreach ($link in $links) {
    $linkPath   = Join-Path $root (Join-Path -Path $link.Link[0] -ChildPath ($link.Link[1..($link.Link.Length-1)] -join '\'))
    $targetPath = Join-Path $root $link.Target
    $parent     = Split-Path -Parent $linkPath
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    if (-not (Test-Path $linkPath)) { New-Item -Path $linkPath -ItemType SymbolicLink -Target $targetPath | Out-Null }
}

foreach ($d in @('build', 'bin\Debug', 'bin\Release')) {
    Copy-Item -Path $dxilSource -Destination (Join-Path $root "$d\dxil.dll") -Force
}

Read-Host "Press Enter to continue..."
