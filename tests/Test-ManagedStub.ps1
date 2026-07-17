param(
    [Parameter(Mandatory = $true)]
    [string]$StubPath
)

$ErrorActionPreference = "Stop"
$StubPath = (Resolve-Path -LiteralPath $StubPath).Path
$SystemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$TestRoot = Join-Path $SystemTemp ("DesktopUpdateKit-managed-stub-test-" + [Guid]::NewGuid().ToString("N"))
$ResolvedTestRoot = [IO.Path]::GetFullPath($TestRoot)
if (-not $ResolvedTestRoot.StartsWith($SystemTemp, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to create a stub test outside the system temporary directory."
}

New-Item -ItemType Directory -Path $ResolvedTestRoot | Out-Null
try {
    $Target = Join-Path $ResolvedTestRoot "target.exe"
    $Downloaded = Join-Path $ResolvedTestRoot "downloaded.exe"
    $Backup = Join-Path $ResolvedTestRoot "target.bak"
    $Marker = Join-Path $ResolvedTestRoot "healthy.ok"
    $TransactionPath = Join-Path $ResolvedTestRoot "update.json"
    [IO.File]::WriteAllText($Target, "original")
    [IO.File]::WriteAllText($Downloaded, "tampered replacement")
    $Transaction = [ordered]@{
        ParentProcessId = 999999
        TargetExePath = $Target
        DownloadedExePath = $Downloaded
        BackupExePath = $Backup
        HealthMarkerPath = $Marker
        ParentExitTimeoutSeconds = 1
        HealthTimeoutSeconds = 1
        ExpectedSha256 = ("0" * 64)
    }
    [IO.File]::WriteAllText($TransactionPath, ($Transaction | ConvertTo-Json -Compress))

    $Process = Start-Process -FilePath $StubPath -ArgumentList @("--transaction", $TransactionPath) -PassThru -Wait
    if ($Process.ExitCode -ne 40) {
        throw "Expected hash-mismatch exit code 40, got $($Process.ExitCode)."
    }
    if ([IO.File]::ReadAllText($Target) -ne "original") {
        throw "The managed updater modified the installed target after a hash mismatch."
    }
    if (Test-Path -LiteralPath $Backup) {
        throw "The managed updater created a backup before validating the staged payload."
    }
    Write-Host "Managed updater hash-mismatch transaction passed."
}
finally {
    if (Test-Path -LiteralPath $ResolvedTestRoot) {
        Remove-Item -LiteralPath $ResolvedTestRoot -Recurse -Force
    }
}
