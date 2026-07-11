param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [string]$ConfigPath = "",
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [switch]$Finalize
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $ProjectRoot "release.config.json"
}
$Config = Get-Content -LiteralPath (Resolve-Path -LiteralPath $ConfigPath) -Raw | ConvertFrom-Json
$Version = $Version.TrimStart('v', 'V')
$TagName = "v$Version"
$AssetDirectory = Join-Path $ProjectRoot "build\release-assets\v$Version"
$Assets = @(
    (Join-Path $AssetDirectory $Config.releaseAssetName),
    (Join-Path $AssetDirectory "$($Config.releaseAssetName).sha256"),
    (Join-Path $AssetDirectory "update.json")
)

foreach ($asset in $Assets) {
    if (-not (Test-Path -LiteralPath $asset)) {
        throw "Release asset not found: $asset"
    }
}

gh auth status
gh release view $TagName --repo $Config.repository | Out-Null
gh release upload $TagName @Assets --repo $Config.repository --clobber

if ($Finalize) {
    gh release edit $TagName --repo $Config.repository --draft=false
}

Write-Host "Release assets uploaded for $Config.repository $TagName"
