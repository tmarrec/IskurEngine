# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot

$targets = @(
    "bin",
    "build",
    "data\scenes",
    "third_party\proprietary\Streamline",
    "third_party\proprietary\Agility"
)

$errors = 0

foreach ($relativePath in $targets) {
    $fullPath = Join-Path $repoRoot $relativePath
    if (Test-Path $fullPath) {
        try {
            Remove-Item -Path $fullPath -Recurse -Force
            Write-Host "Removed: $relativePath"
        } catch {
            $errors += 1
            Write-Host "ERROR: Failed to remove '$relativePath': $($_.Exception.Message)"
        }
    } else {
        Write-Host "Skipped (not found): $relativePath"
    }
}

if ($errors -gt 0) {
    exit 1
}

New-Item -ItemType Directory -Path (Join-Path $repoRoot "third_party\proprietary") -Force | Out-Null
Write-Host "Clean completed."
