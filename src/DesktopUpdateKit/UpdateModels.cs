namespace DesktopUpdateKit;

public sealed record UpdateClientOptions(
    string ApplicationId,
    string Repository,
    string ExeAssetName,
    string Sha256AssetName,
    string? TempDirectoryName = null,
    UpdateDownloadOptions? DownloadOptions = null,
    Version? CurrentVersion = null);

/// <summary>
/// Reusable transport policy for full update packages. The client probes HTTP
/// byte-range support before using multiple connections and falls back to a
/// normal single connection when a host does not support it.
/// </summary>
public sealed record UpdateDownloadOptions(
    int MaxConcurrentConnections = 4,
    long MinimumParallelDownloadBytes = 16L * 1024 * 1024,
    int BufferBytes = 512 * 1024,
    TimeSpan? ProbeTimeout = null,
    TimeSpan? IdleTimeout = null,
    int MaxRetriesPerNode = 1)
{
    internal int EffectiveMaxConcurrentConnections => Math.Clamp(MaxConcurrentConnections, 1, 4);

    internal long EffectiveMinimumParallelDownloadBytes => Math.Max(
        1024 * 1024,
        MinimumParallelDownloadBytes);

    internal int EffectiveBufferBytes => Math.Clamp(BufferBytes, 64 * 1024, 1024 * 1024);

    internal TimeSpan EffectiveProbeTimeout => ProbeTimeout is { } probeTimeout && probeTimeout > TimeSpan.Zero
        ? probeTimeout
        : TimeSpan.FromSeconds(5);

    internal TimeSpan EffectiveIdleTimeout => IdleTimeout is { } idleTimeout && idleTimeout > TimeSpan.Zero
        ? idleTimeout
        : TimeSpan.FromSeconds(20);

    internal int EffectiveMaxRetriesPerNode => Math.Clamp(MaxRetriesPerNode, 0, 1);
}

/// <summary>
/// An update-package transport. The template must contain exactly one
/// <c>{url}</c> marker, which is replaced with the tagged official GitHub
/// Release asset URL.
/// </summary>
public sealed record UpdateDownloadNode(
    string Id,
    string Template,
    int Priority,
    bool Enabled = true);

public sealed record UpdateRelease(
    Version Version,
    string TagName,
    string ReleasePageUrl,
    string ReleaseNotes,
    string ExeDownloadUrl,
    string Sha256DownloadUrl,
    long? ExeSize,
    string ExpectedSha256,
    IReadOnlyList<UpdateDownloadNode>? DownloadNodes = null);

public sealed record UpdateDownloadProgress(
    long BytesReceived,
    long? TotalBytes,
    double BytesPerSecond,
    string? NodeId = null,
    int ActiveConnectionCount = 1,
    bool IsParallelFallback = false)
{
    public double? Fraction => TotalBytes is > 0
        ? (double)BytesReceived / TotalBytes.Value
        : null;
}

public enum UpdateNodeSwitchRequest
{
    None,
    UseAccelerationNodes,
    NextAcceleratedNode,
    UseGitHubDirect
}

public sealed class UpdateDownloadControl
{
    private readonly object _sync = new();
    private TaskCompletionSource? _resumeSignal;
    private CancellationTokenSource _nodeSwitchCancellation = new();
    private bool _useAccelerationNodes;

    public UpdateDownloadControl(bool useAccelerationNodes = true)
    {
        _useAccelerationNodes = useAccelerationNodes;
    }

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

    public bool UseAccelerationNodes
    {
        get
        {
            lock (_sync)
            {
                return _useAccelerationNodes;
            }
        }
    }

    public void SetUseAccelerationNodes(bool enabled)
    {
        lock (_sync)
        {
            if (_useAccelerationNodes == enabled)
            {
                return;
            }

            _useAccelerationNodes = enabled;
            _nodeSwitchCancellation.Cancel();
        }
    }

    public bool RequestNextAcceleratedNode()
    {
        lock (_sync)
        {
            if (!_useAccelerationNodes || _nodeSwitchCancellation.IsCancellationRequested)
            {
                return false;
            }

            _nodeSwitchCancellation.Cancel();
            return true;
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

    internal CancellationToken GetNodeSwitchToken()
    {
        lock (_sync)
        {
            return _nodeSwitchCancellation.Token;
        }
    }

    internal UpdateNodeSwitchRequest ConsumeNodeSwitchRequest(bool wasUsingAccelerationNodes)
    {
        lock (_sync)
        {
            if (!_nodeSwitchCancellation.IsCancellationRequested)
            {
                return UpdateNodeSwitchRequest.None;
            }

            var request = _useAccelerationNodes == wasUsingAccelerationNodes
                ? (_useAccelerationNodes
                    ? UpdateNodeSwitchRequest.NextAcceleratedNode
                    : UpdateNodeSwitchRequest.None)
                : (_useAccelerationNodes
                    ? UpdateNodeSwitchRequest.UseAccelerationNodes
                    : UpdateNodeSwitchRequest.UseGitHubDirect);
            _nodeSwitchCancellation.Dispose();
            _nodeSwitchCancellation = new CancellationTokenSource();
            return request;
        }
    }
}

public sealed record UpdateTransaction(
    int ParentProcessId,
    string TargetExePath,
    string DownloadedExePath,
    string BackupExePath,
    string HealthMarkerPath,
    int ParentExitTimeoutSeconds = 30,
    int HealthTimeoutSeconds = 30,
    string? ExpectedSha256 = null);
