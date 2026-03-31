# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$scriptRoot = Get-ScriptRoot
$repoRoot = Get-RepoRoot
$extractScript = Join-Path $scriptRoot "extract_scenes.ps1"
$depsScript = Join-Path $scriptRoot "setup_proprietary_deps.ps1"
$streamlineDir = Join-Path $repoRoot "third_party\proprietary\Streamline"
$agilityDir = Join-Path $repoRoot "third_party\proprietary\Agility"
$streamlineStamp = Join-Path $streamlineDir "STREAMLINE_SDK_VERSION.txt"
$agilityStamp = Join-Path $agilityDir "AGILITY_SDK_VERSION.txt"
$sceneArchiveUrl = "https://iskurengine-data.old-king-9463.workers.dev/scenes.zip"
$tempSceneArchivePath = Join-Path ([System.IO.Path]::GetTempPath()) ("iskurengine-scenes-" + [System.Guid]::NewGuid().ToString("N") + ".zip")

if (-not (Test-Path $extractScript)) {
    throw "Missing setup script: $extractScript"
}
if (-not (Test-Path $depsScript)) {
    throw "Missing setup script: $depsScript"
}

if ((Test-Path $streamlineStamp) -and (Test-Path $agilityStamp)) {
    Write-Host "Proprietary dependencies already present. Skipping."
} else {
    Write-Host "Proprietary dependencies not found. Running setup..."
    & $depsScript

    $depsExit = Get-LastExitCodeOrZero
    if ($depsExit -ne 0) {
        exit $depsExit
    }
}

try {
    $sceneArchiveSizeBytes = Get-RemoteContentLength -Url $sceneArchiveUrl
    if ($null -ne $sceneArchiveSizeBytes) {
        Write-Host ("Downloading scene archive ({0})..." -f (Format-ByteSize -Bytes $sceneArchiveSizeBytes))
    }
    else {
        Write-Host "Downloading scene archive..."
    }

    $downloadedSceneArchiveSizeBytes = Download-FileWithProgress -Url $sceneArchiveUrl -OutFile $tempSceneArchivePath -Activity "Downloading scene archive" -ExpectedBytes $sceneArchiveSizeBytes
    Write-Host ("Downloaded scene archive size: {0}" -f (Format-ByteSize -Bytes $downloadedSceneArchiveSizeBytes))

    Write-Host "Extracting scenes..."
    & $extractScript -ZipPath $tempSceneArchivePath
    exit (Get-LastExitCodeOrZero)
}
finally {
    if (Test-Path $tempSceneArchivePath) {
        Remove-Item -LiteralPath $tempSceneArchivePath -Force
    }
}
