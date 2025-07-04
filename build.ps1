# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

param([switch]$Elevated)

function New-Folder {
    param (
        [string]$folderName
    )
    
    $folderPath = Join-Path -Path $PSScriptRoot -ChildPath $folderName

    if (-not (Test-Path -Path $folderPath)) {
        New-Item -ItemType Directory -Path $folderPath | Out-Null
        Write-Host "Folder created: `"$folderPath`""
    } else {
        Write-Host "Folder already exists: `"$folderPath`""
    }
}

function New-Symlink {
    param (
        [string]$linkName,
        [string]$targetName
    )

    $linkPath   = Join-Path -Path $PSScriptRoot -ChildPath $linkName
    $targetPath = Join-Path -Path $PSScriptRoot -ChildPath $targetName

    if (-not (Test-Path -Path $linkPath)) {
        New-Item -Path $linkPath -ItemType SymbolicLink -Target $targetPath | Out-Null
        Write-Host "Symbolic link created from `"$linkPath`" to `"$targetPath`""
    } else {
        Write-Host "Symbolic link already exists: `"$linkPath`""
    }
}

function Test-Admin {
    $currentUser = New-Object Security.Principal.WindowsPrincipal $([Security.Principal.WindowsIdentity]::GetCurrent())
    return $currentUser.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# -------------------------------------------------------------------------------
# Starting the build script
Write-Host "`n=== Starting Iskur Engine Build Script ===" -ForegroundColor Cyan

# -------------------------------------------------------------------------------
# Starting log execution time
$startTime = Get-Date

# -------------------------------------------------------------------------------
# Checking Administrator privileges
Write-Host "`n=== Checking for Administrator privileges ===" -ForegroundColor Cyan


if (-not (Test-Admin)) {
    Write-Warning "Not running as administrator."
    if ($Elevated) {
        # Tried to elevate, did not work, abort
        Write-Error "Elevation attempt failed. Aborting build."
        exit 1
    } else {
        Write-Host "Attempting to re-launch script with administrative privileges..." -ForegroundColor Yellow
        Start-Process powershell.exe -Verb RunAs -ArgumentList ('-noprofile -noexit -file "{0}" -elevated' -f ($myinvocation.MyCommand.Definition))
        exit
    }
} else {
    Write-Host "[OK] Running with administrator privileges." -ForegroundColor Green
}

# -------------------------------------------------------------------------------
# VCPKG / Environment Checks
Write-Host "`n=== Verifying VCPKG_ROOT and environment variables ===" -ForegroundColor Cyan

if (-not $env:VCPKG_ROOT) {
    Write-Warning "VCPKG_ROOT environment variable is not set."
    Write-Host "Please set VCPKG_ROOT to your vcpkg installation path and re-run." -ForegroundColor Yellow
    exit 1
} else {
    Write-Host "[OK] VCPKG_ROOT is set to '$($env:VCPKG_ROOT)'." -ForegroundColor Green
}

# -------------------------------------------------------------------------------
# Check that the Windows SDK path exists
Write-Host "`n=== Verifying Windows SDK installation ===" -ForegroundColor Cyan
$winKitsPath = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'
Write-Host "Checking for Windows SDK at '$winKitsPath'..." -ForegroundColor Gray
if (-not (Test-Path $winKitsPath)) {
    Write-Error "ERROR: Windows SDK path '$winKitsPath' not found.`nPlease ensure the Windows SDK (version 10.0.26100.X) is installed.`nYou can add it via Visual Studio Installer -> Modify your VS installation -> Individual Components -> select 'Windows 11 SDK (10.0.26100.X)'."
    exit 1
} else {
    Write-Host "[OK] Windows SDK path found: '$winKitsPath'." -ForegroundColor Green
}
$dxilSource = Join-Path -Path $winKitsPath -ChildPath 'dxil.dll'

# -------------------------------------------------------------------------------
# Check that LLVM/Clang component is present in Visual Studio
Write-Host "`n=== Checking for LLVM/Clang component in Visual Studio ===" -ForegroundColor Cyan
$vswherePath = Join-Path -Path ${env:ProgramFiles(x86)} -ChildPath 'Microsoft Visual Studio\Installer\vswhere.exe'
Write-Host "Looking for vswhere.exe at '$vswherePath'..." -ForegroundColor Gray
if (-not (Test-Path $vswherePath)) {
    Write-Error "ERROR: Cannot find vswhere.exe at '$vswherePath'. Make sure Visual Studio Installer is installed."
    exit 1
} else {
    Write-Host "[OK] vswhere.exe found." -ForegroundColor Green
}

Write-Host "Checking for Clang-pack component via vswhere..." -ForegroundColor Gray
$clangComponentInfo = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath 2> $null
if ([string]::IsNullOrWhiteSpace($clangComponentInfo)) {
    Write-Warning "Clang-pack (LLVM/Clang) is NOT installed in your Visual Studio instance."
    exit 1
} else {
    Write-Host "[OK] Clang-pack (LLVM/Clang) is installed under: $clangComponentInfo" -ForegroundColor Green
}

# -------------------------------------------------------------------------------
# Ensure required folders exist
Write-Host "`n=== Ensuring required folders are created ===" -ForegroundColor Cyan

if (-not (Test-Path "$PSScriptRoot\build")) {
    Write-Host "Creating 'build' directory..." -ForegroundColor Yellow
    New-Item -ItemType Directory -Path "$PSScriptRoot\build" | Out-Null
} else {
    Write-Host "'build' directory already exists." -ForegroundColor Gray
}

# -------------------------------------------------------------------------------
# Configuring with CMake
Write-Host "`n=== Configuring project with CMake ===" -ForegroundColor Cyan
Write-Host "-- Running: cmake -S $PSScriptRoot --preset=default" -ForegroundColor Gray
cmake -S $PSScriptRoot --preset=default
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed (exit code $LASTEXITCODE)."
    exit $LASTEXITCODE
}
Write-Host "[OK] CMake configuration succeeded." -ForegroundColor Green

# -------------------------------------------------------------------------------
# Data linking
Write-Host "`n=== Data linking ===" -ForegroundColor Cyan

New-Folder "build\Debug"
Write-Host "[OK] Ensured folder build\Debug." -ForegroundColor Green

New-Folder "build\Release"
Write-Host "[OK] Ensured folder build\Release." -ForegroundColor Green

New-Symlink "build\data" "data"
Write-Host "[OK] Created symlink build\data -> data." -ForegroundColor Green

New-Symlink "build\Debug\data" "data"
Write-Host "[OK] Created symlink build\Debug\data -> data." -ForegroundColor Green

New-Symlink "build\Release\data" "data"
Write-Host "[OK] Created symlink build\Release\data -> data." -ForegroundColor Green

Copy-Item -Path $dxilSource -Destination (Join-Path -Path $PSScriptRoot -ChildPath 'build\dxil.dll')
Write-Host "[OK] Copied dxil.dll to build\dxil.dll." -ForegroundColor Green

Copy-Item -Path $dxilSource -Destination (Join-Path -Path $PSScriptRoot -ChildPath 'build\Debug\dxil.dll')
Write-Host "[OK] Copied dxil.dll to build\Debug\dxil.dll." -ForegroundColor Green

Copy-Item -Path $dxilSource -Destination (Join-Path -Path $PSScriptRoot -ChildPath 'build\Release\dxil.dll')
Write-Host "[OK] Copied dxil.dll to build\Release\dxil.dll." -ForegroundColor Green

# -------------------------------------------------------------------------------
# Finalization
Write-Host "`n=== Build completed successfully ===" -ForegroundColor Cyan
Write-Host "== COMPLETED ==" -ForegroundColor Green

# -------------------------------------------------------------------------------
# Calculate and log execution time
$endTime   = Get-Date
$duration  = $endTime - $startTime
Write-Host "`nTotal execution time: $([int]$duration.TotalMinutes) minutes, $($duration.Seconds) seconds." -ForegroundColor Yellow

Pause
Stop-Process -Id $PID
