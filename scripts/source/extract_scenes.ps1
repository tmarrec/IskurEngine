# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

param(
    [Parameter(Mandatory = $true)]
    [string]$ZipPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot "common.ps1")

$root = Get-RepoRoot
$dst  = Join-Path $root 'data\scenes'

if (-not (Test-Path $ZipPath)) {
    throw "Scene archive not found: $ZipPath"
}

$null = New-Item -ItemType Directory -Path $dst -Force | Out-Null

Expand-Archive -LiteralPath $ZipPath -DestinationPath $dst -Force
Write-Host "Extracted scene archive to: $dst"
