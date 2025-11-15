# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'data\scenes'
$dst  = Join-Path $root 'data\scenes_compressed'

if (-not (Test-Path $src)) { throw "Source folder not found: $src" }
$null = New-Item -ItemType Directory -Path $dst -Force | Out-Null

$files = Get-ChildItem -LiteralPath $src -File
if (-not $files) { Write-Host "No files in $src"; exit 0 }

foreach ($f in $files) {
    $outName = "$($f.Name).tar.gz"
    $outPath = Join-Path $dst $outName
    if (Test-Path $outPath) { Remove-Item -LiteralPath $outPath -Force }

    & tar -c --gzip --options "gzip:compression-level=9" -f $outPath -C $($f.DirectoryName) $($f.Name)
    if ($LASTEXITCODE -ne 0) {
        throw "tar (gzip) failed for '$($f.Name)' with exit code $LASTEXITCODE."
    }

    Write-Host "Wrote $outPath"
}

Write-Host "Done. Archives are in: $dst"