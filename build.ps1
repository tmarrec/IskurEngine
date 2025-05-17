# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

# START RUN ADMIN #######################################################################################
param([switch]$Elevated)

function Test-Admin {
    $currentUser = New-Object Security.Principal.WindowsPrincipal $([Security.Principal.WindowsIdentity]::GetCurrent())
    $currentUser.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
}

if ((Test-Admin) -eq $false)  {
    if ($elevated) {
        # tried to elevate, did not work, aborting
    } else {
        Start-Process powershell.exe -Verb RunAs -ArgumentList ('-noprofile -noexit -file "{0}" -elevated' -f ($myinvocation.MyCommand.Definition))
    }
    exit 0
}
# END RUN ADMIN #######################################################################################

function Create-Folder {
    param (
        [string]$folderName
    )
    
    $folderPath = Join-Path -Path $PSScriptRoot -ChildPath $folderName

    if (-not (Test-Path -Path $folderPath)) {
        New-Item -ItemType Directory -Path $folderPath | Out-Null
        Write-Host "Folder created: ""$folderPath"""
    } else {
        Write-Host "Folder already exists: ""$folderPath"""
    }
}

function Create-Symlink {
    param (
        [string]$linkName,
        [string]$targetName
    )

    $linkPath = Join-Path -Path $PSScriptRoot -ChildPath $linkName
    $targetPath = Join-Path -Path $PSScriptRoot -ChildPath $targetName

    if (-not (Test-Path -Path $linkPath)) {
        New-Item -Path $linkPath -ItemType SymbolicLink -Target $targetPath | Out-Null
        Write-Host "Symbolic link created from ""$linkPath"" to ""$targetPath"""
    } else {
        Write-Host "Symbolic link already exists: ""$linkPath"""
    }
}

# Cmake
Write-Host "== Running CMake ==" -ForegroundColor Green
cmake $PSScriptRoot --preset=default

# Data linking
Write-Host "== Data linking ==" -ForegroundColor Green
Create-Folder "build\Debug"
Create-Folder "build\Release"
Create-Symlink "build\data" "data"
Create-Symlink "build\Debug\data" "data"
Create-Symlink "build\Release\data" "data"

Copy-Item -Path "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll" -Destination (Join-Path -Path $PSScriptRoot -ChildPath "build\dxil.dll")
Copy-Item -Path "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll" -Destination (Join-Path -Path $PSScriptRoot -ChildPath "build\Debug\dxil.dll")
Copy-Item -Path "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxil.dll" -Destination (Join-Path -Path $PSScriptRoot -ChildPath "build\Release\dxil.dll")

Write-Host "== COMPLETED ==" -ForegroundColor Green
Pause
Stop-Process -Id $PID