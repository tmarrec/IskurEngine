# Iskur Engine
# Copyright (c) 2025 Tristan Marrec
# Licensed under the MIT License.
# See the LICENSE file in the project root for license information.

Set-StrictMode -Version Latest

function Get-ScriptRoot {
    if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        return $PSScriptRoot
    }
    return Split-Path -Parent $MyInvocation.MyCommand.Definition
}

function Get-RepoRoot {
    return Join-Path (Get-ScriptRoot) "..\.."
}

function Get-LastExitCodeOrZero {
    if (Get-Variable -Name LASTEXITCODE -ErrorAction SilentlyContinue) {
        return [int]$LASTEXITCODE
    }
    return 0
}

function New-HttpRequest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Url,

        [Parameter(Mandatory = $true)]
        [string]$Method
    )

    $request = [System.Net.HttpWebRequest][System.Net.WebRequest]::Create($Url)
    $request.Method = $Method
    $request.AllowAutoRedirect = $true
    $request.AutomaticDecompression = [System.Net.DecompressionMethods]::GZip -bor [System.Net.DecompressionMethods]::Deflate
    return $request
}

function Get-ByteSizeUnitIndex {
    param(
        [Parameter(Mandatory = $true)]
        [long]$ReferenceBytes,

        [int]$MinimumUnitIndex = 0
    )

    $maxUnitIndex = 4
    $unitIndex = [Math]::Max(0, $MinimumUnitIndex)
    $size = [double]$ReferenceBytes / [Math]::Pow(1024, $unitIndex)

    while ($size -ge 1024 -and $unitIndex -lt $maxUnitIndex) {
        $size /= 1024
        $unitIndex++
    }

    return $unitIndex
}

function Format-ByteSizeWithUnitIndex {
    param(
        [Parameter(Mandatory = $true)]
        [long]$Bytes,

        [Parameter(Mandatory = $true)]
        [int]$UnitIndex
    )

    $units = @("B", "KB", "MB", "GB", "TB")
    $safeUnitIndex = [Math]::Max(0, [Math]::Min($UnitIndex, $units.Length - 1))
    $size = [double]$Bytes / [Math]::Pow(1024, $safeUnitIndex)

    if ($safeUnitIndex -eq 0) {
        return ("{0} {1}" -f [long]$size, $units[$safeUnitIndex])
    }

    return ("{0:N1} {1}" -f $size, $units[$safeUnitIndex])
}

function Format-TransferRate {
    param(
        [Parameter(Mandatory = $true)]
        [double]$BytesPerSecond,

        [Parameter(Mandatory = $true)]
        [int]$UnitIndex
    )

    if ($BytesPerSecond -le 0) {
        return $null
    }

    return ("{0}/s" -f (Format-ByteSizeWithUnitIndex -Bytes ([long][Math]::Round($BytesPerSecond)) -UnitIndex $UnitIndex))
}

function Format-Eta {
    param(
        [Parameter(Mandatory = $true)]
        [double]$Seconds
    )

    $eta = [TimeSpan]::FromSeconds([Math]::Ceiling([Math]::Max(0, $Seconds)))
    if ($eta.TotalHours -ge 1) {
        return $eta.ToString("hh\:mm\:ss")
    }

    return $eta.ToString("mm\:ss")
}

function Format-ByteSize {
    param(
        [Parameter(Mandatory = $true)]
        [long]$Bytes
    )

    $units = @("B", "KB", "MB", "GB", "TB")
    $size = [double]$Bytes
    $unitIndex = 0

    while ($size -ge 1024 -and $unitIndex -lt ($units.Length - 1)) {
        $size /= 1024
        $unitIndex++
    }

    if ($unitIndex -eq 0) {
        return ("{0} {1}" -f [long]$size, $units[$unitIndex])
    }

    return ("{0:N1} {1}" -f $size, $units[$unitIndex])
}

function Format-DownloadProgressStatus {
    param(
        [Parameter(Mandatory = $true)]
        [long]$DownloadedBytes,

        [Nullable[long]]$TotalBytes = $null,

        [double]$BytesPerSecond = 0
    )

    $progressUnitIndex = 2

    if ($null -ne $TotalBytes -and $TotalBytes -gt 0) {
        $downloaded = Format-ByteSizeWithUnitIndex -Bytes $DownloadedBytes -UnitIndex $progressUnitIndex
        $total = Format-ByteSizeWithUnitIndex -Bytes $TotalBytes -UnitIndex $progressUnitIndex
        $percent = [Math]::Min(([double]$DownloadedBytes * 100.0) / [double]$TotalBytes, 100.0)
        $status = "{0} / {1} ({2:N1}%)" -f $downloaded, $total, $percent
        $speed = Format-TransferRate -BytesPerSecond $BytesPerSecond -UnitIndex $progressUnitIndex
        if ($null -ne $speed) {
            $status = "{0} - {1}" -f $status, $speed

            $remainingBytes = [Math]::Max([long]0, ([long]$TotalBytes - [long]$DownloadedBytes))
            if ($remainingBytes -gt 0) {
                $eta = Format-Eta -Seconds ($remainingBytes / $BytesPerSecond)
                $status = "{0} - ETA {1}" -f $status, $eta
            }
        }

        return $status
    }

    $status = "{0} downloaded" -f (Format-ByteSizeWithUnitIndex -Bytes $DownloadedBytes -UnitIndex $progressUnitIndex)
    $speed = Format-TransferRate -BytesPerSecond $BytesPerSecond -UnitIndex $progressUnitIndex
    if ($null -ne $speed) {
        $status = "{0} - {1}" -f $status, $speed
    }

    return $status
}

function Write-DownloadProgress {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Activity,

        [Parameter(Mandatory = $true)]
        [long]$DownloadedBytes,

        [Nullable[long]]$TotalBytes = $null,

        [double]$BytesPerSecond = 0,

        [switch]$Completed
    )

    $status = Format-DownloadProgressStatus -DownloadedBytes $DownloadedBytes -TotalBytes $TotalBytes -BytesPerSecond $BytesPerSecond
    if ($null -ne $TotalBytes -and $TotalBytes -gt 0) {
        $percentComplete = [int][Math]::Min(([double]$DownloadedBytes * 100.0) / [double]$TotalBytes, 100.0)
        Write-Progress -Activity $Activity -Status $status -PercentComplete $percentComplete
    }
    else {
        Write-Progress -Activity $Activity -Status $status -PercentComplete -1
    }

    if ($Completed) {
        Write-Progress -Activity $Activity -Completed
    }
}

function Get-RemoteContentLength {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Url
    )

    try {
        $request = New-HttpRequest -Url $Url -Method "HEAD"
        $response = [System.Net.HttpWebResponse]$request.GetResponse()
        try {
            if ($response.ContentLength -ge 0) {
                return [long]$response.ContentLength
            }
        }
        finally {
            $response.Dispose()
        }
    }
    catch {
        return $null
    }

    return $null
}

function Download-FileWithProgress {
    param(
        [Parameter(Mandatory = $true)]
        [Alias("Url")]
        [string]$SourceUrl,

        [Parameter(Mandatory = $true)]
        [Alias("OutFile")]
        [string]$DestinationPath,

        [Parameter(Mandatory = $true)]
        [string]$Activity,

        [Alias("ExpectedBytes")]
        [Nullable[long]]$KnownTotalBytes = $null
    )

    $request = New-HttpRequest -Url $SourceUrl -Method "GET"
    $response = [System.Net.HttpWebResponse]$request.GetResponse()
    try {
        $totalBytes = $KnownTotalBytes
        if (($null -eq $totalBytes -or $totalBytes -le 0) -and $response.ContentLength -ge 0) {
            $totalBytes = [long]$response.ContentLength
        }

        $responseStream = $response.GetResponseStream()
        if ($null -eq $responseStream) {
            throw "Failed to read response stream for $SourceUrl"
        }

        try {
            $outDir = Split-Path -Parent $DestinationPath
            if (-not [string]::IsNullOrWhiteSpace($outDir) -and -not (Test-Path -LiteralPath $outDir)) {
                New-Item -ItemType Directory -Path $outDir -Force | Out-Null
            }

            $fileStream = [System.IO.File]::Open($DestinationPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
            try {
                $buffer = New-Object byte[] (1024 * 1024)
                $downloadedBytes = 0L
                $throughputTimer = [System.Diagnostics.Stopwatch]::StartNew()
                $progressTimer = [System.Diagnostics.Stopwatch]::StartNew()
                Write-DownloadProgress -Activity $Activity -DownloadedBytes 0 -TotalBytes $totalBytes -BytesPerSecond 0

                while (($bytesRead = $responseStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $fileStream.Write($buffer, 0, $bytesRead)
                    $downloadedBytes += $bytesRead

                    if ($progressTimer.ElapsedMilliseconds -ge 200) {
                        $elapsedSeconds = [Math]::Max(0.01, $throughputTimer.Elapsed.TotalSeconds)
                        $bytesPerSecond = $downloadedBytes / $elapsedSeconds
                        Write-DownloadProgress -Activity $Activity -DownloadedBytes $downloadedBytes -TotalBytes $totalBytes -BytesPerSecond $bytesPerSecond
                        $progressTimer.Restart()
                    }
                }

                $elapsedSeconds = [Math]::Max(0.01, $throughputTimer.Elapsed.TotalSeconds)
                $bytesPerSecond = $downloadedBytes / $elapsedSeconds
                Write-DownloadProgress -Activity $Activity -DownloadedBytes $downloadedBytes -TotalBytes $totalBytes -BytesPerSecond $bytesPerSecond -Completed
                return $downloadedBytes
            }
            finally {
                $fileStream.Dispose()
            }
        }
        finally {
            $responseStream.Dispose()
        }
    }
    finally {
        $response.Dispose()
    }
}
