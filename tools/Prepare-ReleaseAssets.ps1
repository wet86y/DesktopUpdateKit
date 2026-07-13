param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [string]$ConfigPath = "",
    [string]$Version = "",
    [string]$ReleaseNotes = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $ProjectRoot "release.config.json"
}
$Config = Get-Content -LiteralPath (Resolve-Path -LiteralPath $ConfigPath) -Raw | ConvertFrom-Json
. (Join-Path $PSScriptRoot "ReleaseRules.ps1")
Assert-ReleaseConfig -Config $Config
$PublishedExe = Join-Path (Join-Path $ProjectRoot $Config.artifactsDirectory) $Config.publishedExeName

if (-not (Test-Path -LiteralPath $PublishedExe)) {
    throw "Published EXE not found. Run the shared Build-Release.ps1 first."
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Item -LiteralPath $PublishedExe).VersionInfo.ProductVersion
}

$Version = $Version.TrimStart('v', 'V').Split('+', 2)[0]
$AssetDirectory = Join-Path $ProjectRoot "build\release-assets\v$Version"
if (Test-Path -LiteralPath $AssetDirectory) {
    Remove-Item -LiteralPath $AssetDirectory -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $AssetDirectory | Out-Null

$AssetName = $Config.releaseAssetName
$AssetPath = Join-Path $AssetDirectory $AssetName
Copy-Item -LiteralPath $PublishedExe -Destination $AssetPath

$LicenseAssets = @{
    "LICENSE" = (Join-Path $ProjectRoot "LICENSE")
    "NOTICE" = (Join-Path $ProjectRoot "NOTICE")
    "THIRD-PARTY-NOTICES.md" = (Join-Path $ProjectRoot "THIRD-PARTY-NOTICES.md")
    "DesktopUpdateKit-LICENSE.txt" = (Join-Path $PSScriptRoot "..\LICENSE")
}
foreach ($entry in $LicenseAssets.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value)) {
        throw "Required license file not found: $($entry.Value)"
    }
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $AssetDirectory $entry.Key)
}

$Hash = (Get-FileHash -LiteralPath $AssetPath -Algorithm SHA256).Hash.ToLowerInvariant()
$Sha256Name = "$AssetName.sha256"
Set-Content -LiteralPath (Join-Path $AssetDirectory $Sha256Name) -Value "$Hash  $AssetName" -Encoding ascii

if ([string]::IsNullOrWhiteSpace($ReleaseNotes)) {
    $ReleaseNotes = "See the GitHub Release page for update details."
}

$Repository = $Config.repository
$TagName = "v$Version"
$Manifest = [ordered]@{
    version = $Version
    channel = "stable"
    repository = $Repository
    tag = $TagName
    asset = $AssetName
    sha256Asset = $Sha256Name
    sha256 = $Hash
    size = (Get-Item -LiteralPath $AssetPath).Length
    downloadUrl = "https://github.com/$Repository/releases/latest/download/$AssetName"
    sha256Url = "https://github.com/$Repository/releases/latest/download/$Sha256Name"
    releaseNotesUrl = "https://github.com/$Repository/releases/tag/$TagName"
    releaseNotes = $ReleaseNotes
}
if ($null -ne $Config.downloadNodes) {
    $Manifest.downloadNodes = @($Config.downloadNodes)
}
$Manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $AssetDirectory "update.json") -Encoding utf8
Assert-PreparedReleaseAssets -AssetDirectory $AssetDirectory -Config $Config -Version $Version

Write-Host "Release assets prepared: $AssetDirectory"
