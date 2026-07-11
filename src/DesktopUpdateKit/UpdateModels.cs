namespace DesktopUpdateKit;

public sealed record UpdateClientOptions(
    string ApplicationId,
    string Repository,
    string ExeAssetName,
    string Sha256AssetName,
    string? TempDirectoryName = null);

public sealed record UpdateRelease(
    Version Version,
    string TagName,
    string ReleasePageUrl,
    string ReleaseNotes,
    string ExeDownloadUrl,
    string Sha256DownloadUrl,
    long? ExeSize);

public sealed record UpdateTransaction(
    int ParentProcessId,
    string TargetExePath,
    string DownloadedExePath,
    string BackupExePath,
    string HealthMarkerPath,
    int ParentExitTimeoutSeconds = 30,
    int HealthTimeoutSeconds = 30);
