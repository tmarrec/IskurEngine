param(
    [Parameter(Mandatory = $false)]
    [string]$StreamlineUrl = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.10.3/streamline-sdk-v2.10.3.zip",

    [Parameter(Mandatory = $false)]
    [string]$StreamlineOutDir,

    [Parameter(Mandatory = $false)]
    [string]$StreamlineCacheDir,

    [Parameter(Mandatory = $false)]
    [string]$AgilityPackageId = "microsoft.direct3d.d3d12",

    [Parameter(Mandatory = $false)]
    [string]$AgilityVersion = "1.619.1",

    [Parameter(Mandatory = $false)]
    [string]$AgilityOutDir,

    [Parameter(Mandatory = $false)]
    [string]$AgilityCacheDir,

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot
if ([string]::IsNullOrWhiteSpace($StreamlineOutDir)) {
    $StreamlineOutDir = Join-Path $repoRoot "third_party\proprietary\Streamline"
}
if ([string]::IsNullOrWhiteSpace($StreamlineCacheDir)) {
    $StreamlineCacheDir = Join-Path $repoRoot "build\streamline_cache"
}
if ([string]::IsNullOrWhiteSpace($AgilityOutDir)) {
    $AgilityOutDir = Join-Path $repoRoot "third_party\proprietary\Agility"
}
if ([string]::IsNullOrWhiteSpace($AgilityCacheDir)) {
    $AgilityCacheDir = Join-Path $repoRoot "build\agility_cache"
}

function Fail([string]$Message) {
    Write-Host "ERROR: $Message"
    exit 1
}

function Get-StagedValue([string]$Dir, [string]$StampName, [string]$Key) {
    $stamp = Join-Path $Dir $StampName
    if (-not (Test-Path $stamp)) { return $null }
    $lines = Get-Content -Path $stamp -ErrorAction SilentlyContinue
    foreach ($line in $lines) {
        $prefix = "$Key="
        if ($line -like "$prefix*") {
            return $line.Substring($prefix.Length)
        }
    }
    return $null
}

function Get-StagedUrl([string]$Dir, [string]$StampName) {
    return Get-StagedValue -Dir $Dir -StampName $StampName -Key "url"
}

function Stage-Streamline {
    $stampName = "STREAMLINE_SDK_VERSION.txt"
    if (Test-Path $StreamlineOutDir) {
        $stagedUrl = Get-StagedUrl -Dir $StreamlineOutDir -StampName $stampName
        if ((-not $Force) -and $stagedUrl -and ($stagedUrl -eq $StreamlineUrl)) {
            Write-Host "Streamline SDK already staged and matches URL. Skipping."
            return
        }
        if (-not $Force) {
            Fail "Streamline SDK already staged but does not match URL. Use -Force to overwrite."
        }
        Remove-Item -Path $StreamlineOutDir -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $StreamlineOutDir | Out-Null
    New-Item -ItemType Directory -Force -Path $StreamlineCacheDir | Out-Null

    $zipName = [IO.Path]::GetFileName($StreamlineUrl)
    if ([string]::IsNullOrWhiteSpace($zipName)) {
        $zipName = "streamline_sdk.zip"
    }

    $zipPath = Join-Path $StreamlineCacheDir $zipName
    $extractDir = Join-Path $StreamlineCacheDir "extract"

    if (Test-Path $extractDir) {
        Remove-Item -Path $extractDir -Recurse -Force
    }

    Write-Host "Downloading Streamline SDK..."
    Download-FileWithProgress -SourceUrl $StreamlineUrl -DestinationPath $zipPath -Activity "Downloading Streamline SDK"

    Write-Host "Extracting Streamline SDK..."
    Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

    $rootEntries = Get-ChildItem -Path $extractDir
    if ($rootEntries.Count -eq 1 -and $rootEntries[0].PSIsContainer) {
        $root = $rootEntries[0].FullName
    } else {
        $root = $extractDir
    }

    Write-Host "Staging Streamline SDK to $StreamlineOutDir..."
    Copy-Item -Path (Join-Path $root "*") -Destination $StreamlineOutDir -Recurse -Force

    $stamp = Join-Path $StreamlineOutDir $stampName
    $stampLines = @(
        "url=$StreamlineUrl",
        "timestamp=$(Get-Date -Format o)"
    )
    Set-Content -Path $stamp -Value $stampLines -Encoding UTF8

    Write-Host "Done. Streamline SDK staged at: $StreamlineOutDir"
}

function Stage-Agility {
    $stampName = "AGILITY_SDK_VERSION.txt"
    if (Test-Path $AgilityOutDir) {
        $stagedPackageId = Get-StagedValue -Dir $AgilityOutDir -StampName $stampName -Key "package_id"
        $stagedVersion = Get-StagedValue -Dir $AgilityOutDir -StampName $stampName -Key "version"
        $isMatch = ($stagedPackageId -eq $AgilityPackageId) -and ($stagedVersion -eq $AgilityVersion)
        if ((-not $Force) -and $isMatch) {
            Write-Host "D3D12 Agility SDK already staged and matches package/version. Skipping."
            return
        }
        if (-not $Force) {
            Fail "D3D12 Agility SDK already staged but does not match package/version. Use -Force to overwrite."
        }
        Remove-Item -Path $AgilityOutDir -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $AgilityOutDir | Out-Null
    New-Item -ItemType Directory -Force -Path $AgilityCacheDir | Out-Null

    $packageIdLower = $AgilityPackageId.ToLowerInvariant()
    $packageFile = "$packageIdLower.$AgilityVersion.nupkg"
    $packageUrl = "https://api.nuget.org/v3-flatcontainer/$packageIdLower/$AgilityVersion/$packageFile"
    $archivePath = Join-Path $AgilityCacheDir "$packageIdLower.$AgilityVersion.zip"
    $extractDir = Join-Path $AgilityCacheDir "extract"

    if (Test-Path $extractDir) {
        Remove-Item -Path $extractDir -Recurse -Force
    }

    Write-Host "Downloading D3D12 Agility SDK..."
    Download-FileWithProgress -SourceUrl $packageUrl -DestinationPath $archivePath -Activity "Downloading D3D12 Agility SDK"

    Write-Host "Extracting D3D12 Agility SDK..."
    Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force

    Write-Host "Staging D3D12 Agility SDK to $AgilityOutDir..."
    Copy-Item -Path (Join-Path $extractDir "*") -Destination $AgilityOutDir -Recurse -Force

    $stamp = Join-Path $AgilityOutDir $stampName
    $stampLines = @(
        "package_id=$AgilityPackageId",
        "version=$AgilityVersion",
        "url=$packageUrl",
        "timestamp=$(Get-Date -Format o)"
    )
    Set-Content -Path $stamp -Value $stampLines -Encoding UTF8

    Write-Host "Done. D3D12 Agility SDK staged at: $AgilityOutDir"
}

Stage-Streamline
Stage-Agility
Write-Host "Proprietary dependency setup completed."
