param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,

    [string]$ConfigPath = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $ProjectRoot "release.config.json"
}
$ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
$Config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
$SharedRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "ReleaseRules.ps1")
Assert-ReleaseConfig -Config $Config

function Resolve-ProjectPath([string]$RelativePath) {
    $candidate = Join-Path $ProjectRoot $RelativePath
    return [IO.Path]::GetFullPath($candidate)
}

$Project = Resolve-ProjectPath $Config.projectFile
$Artifacts = Resolve-ProjectPath $Config.artifactsDirectory
$UpdaterProject = Join-Path $SharedRoot "src\UpdaterStub\UpdaterStub.csproj"
$UpdaterOutput = Resolve-ProjectPath "build\updater\win-x64"
$ExpectedExe = Join-Path $Artifacts $Config.publishedExeName
$EnableSingleFileCompression = $true
if ($null -ne $Config.enableSingleFileCompression) {
    $EnableSingleFileCompression = [bool]$Config.enableSingleFileCompression
}

foreach ($path in @($Project, $UpdaterProject)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Build input not found: $path"
    }
}

Write-Host "Cleaning host Release outputs to prevent stale incremental assemblies..."
dotnet clean $Project -c Release
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

foreach ($path in @($Artifacts, $UpdaterOutput)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $path | Out-Null
}

Write-Host "Restoring shared updater and project..."
dotnet restore $UpdaterProject
dotnet restore $Project

Write-Host "Publishing shared UpdaterStub..."
dotnet publish $UpdaterProject `
    -c Release `
    -r $Config.runtimeIdentifier `
    --self-contained true `
    /p:DebugSymbols=false `
    /p:DebugType=None `
    /p:StripSymbols=true `
    -o $UpdaterOutput

$UpdaterExe = Join-Path $UpdaterOutput "UpdaterStub.exe"
if (-not (Test-Path -LiteralPath $UpdaterExe)) {
    throw "Shared UpdaterStub publish did not produce $UpdaterExe"
}

Write-Host "Publishing project single-file executable..."
$ProjectPublishArguments = @(
    "-c", "Release",
    "-r", $Config.runtimeIdentifier,
    "--self-contained", "true",
    "/p:PublishSingleFile=true",
    "/p:PasteTraceEnabled=false",
    "/p:DebugSymbols=false",
    "/p:DebugType=None",
    "/p:IncludeNativeLibrariesForSelfExtract=true",
    "/p:UpdaterStubPath=$UpdaterExe"
)
if ($EnableSingleFileCompression) {
    $ProjectPublishArguments += "/p:EnableCompressionInSingleFile=true"
}
$ProjectPublishArguments += @("-o", $Artifacts)
& dotnet publish $Project @ProjectPublishArguments

if (-not (Test-Path -LiteralPath $ExpectedExe)) {
    throw "Release executable was not produced: $ExpectedExe"
}

Invoke-ReleaseExecutableVerification `
    -ExecutablePath $ExpectedExe `
    -Arguments $Config.releaseVerificationArguments

Write-Host "Done. Output: $Artifacts"
