# Iškur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'data\scenes_compressed'
$dst  = Join-Path $root 'data\scenes'

if (-not (Test-Path $src)) {
    throw "Source folder not found: $src"
}
$null = New-Item -ItemType Directory -Path $dst -Force | Out-Null

$files = Get-ChildItem -LiteralPath $src -File | Where-Object {
    $_.Name.EndsWith('.tar.gz', 'InvariantCultureIgnoreCase')
}
if (-not $files) {
    Write-Host "No .tar.gz archives in $src"
    exit 0
}

foreach ($f in $files) {
    & tar -x --gzip -f $f.FullName -C $dst
    if ($LASTEXITCODE -ne 0) {
        throw "tar extract failed for '$($f.Name)' with exit code $LASTEXITCODE."
    }
    Write-Host "Extracted $($f.Name)"
}

Write-Host "Done. Extracted to: $dst"
