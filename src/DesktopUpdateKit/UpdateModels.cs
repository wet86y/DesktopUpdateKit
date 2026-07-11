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

public sealed record UpdateDownloadProgress(
    long BytesReceived,
    long? TotalBytes,
    double BytesPerSecond)
{
    public double? Fraction => TotalBytes is > 0
        ? (double)BytesReceived / TotalBytes.Value
        : null;
}

public sealed class UpdateDownloadControl
{
    private readonly object _sync = new();
    private TaskCompletionSource? _resumeSignal;

    public bool IsPaused
    {
        get
        {
            lock (_sync)
            {
                return _resumeSignal is not null;
            }
        }
    }

    public void Pause()
    {
        lock (_sync)
        {
            _resumeSignal ??= new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }

    public void Resume()
    {
        TaskCompletionSource? signal;
        lock (_sync)
        {
            signal = _resumeSignal;
            _resumeSignal = null;
        }

        signal?.TrySetResult();
    }

    public Task WaitIfPausedAsync(CancellationToken cancellationToken)
    {
        Task waitTask;
        lock (_sync)
        {
            if (_resumeSignal is null)
            {
                return Task.CompletedTask;
            }

            waitTask = _resumeSignal.Task;
        }

        return waitTask.WaitAsync(cancellationToken);
    }
}

public sealed record UpdateTransaction(
    int ParentProcessId,
    string TargetExePath,
    string DownloadedExePath,
    string BackupExePath,
    string HealthMarkerPath,
    int ParentExitTimeoutSeconds = 30,
    int HealthTimeoutSeconds = 30);
