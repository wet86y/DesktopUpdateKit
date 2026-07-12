namespace DesktopUpdateKit;

public enum UpdateDownloadSessionState
{
    Idle,
    Downloading,
    Paused,
    Completed,
    Failed,
    Cancelled
}

public sealed record UpdateDownloadSessionSnapshot(
    UpdateDownloadSessionState State,
    UpdateRelease? Release,
    UpdateDownloadProgress? Progress,
    string? DownloadedPath,
    string? ErrorMessage,
    bool ContinueInBackground,
    bool UseAccelerationNodes);

/// <summary>
/// Process-lifetime update download state. UI windows may attach and detach
/// without cancelling an active download; callers explicitly choose whether a
/// closed UI pauses the transfer or lets it continue in the background.
/// </summary>
public sealed class UpdateDownloadSession : IDisposable
{
    private readonly object _sync = new();
    private readonly UpdateClient _client;
    private UpdateDownloadControl? _control;
    private CancellationTokenSource? _cancellation;
    private UpdateDownloadSessionSnapshot _snapshot = new(UpdateDownloadSessionState.Idle, null, null, null, null, false, true);
    private bool _disposed;

    public UpdateDownloadSession(UpdateClient client)
    {
        _client = client ?? throw new ArgumentNullException(nameof(client));
    }

    public event EventHandler<UpdateDownloadSessionSnapshot>? Changed;

    public UpdateDownloadSessionSnapshot Snapshot
    {
        get
        {
            lock (_sync)
            {
                return _snapshot;
            }
        }
    }

    public bool TryStart(UpdateRelease release, bool useAccelerationNodes)
    {
        ArgumentNullException.ThrowIfNull(release);
        lock (_sync)
        {
            ThrowIfDisposed();
            if (_snapshot.State is UpdateDownloadSessionState.Downloading or UpdateDownloadSessionState.Paused)
            {
                return false;
            }

            _cancellation?.Dispose();
            _cancellation = new CancellationTokenSource();
            _control = new UpdateDownloadControl();
            _control.SetUseAccelerationNodes(useAccelerationNodes);
            _snapshot = new UpdateDownloadSessionSnapshot(
                UpdateDownloadSessionState.Downloading,
                release,
                Progress: null,
                DownloadedPath: null,
                ErrorMessage: null,
                ContinueInBackground: false,
                UseAccelerationNodes: useAccelerationNodes);
            _ = RunAsync(release, _control, _cancellation);
        }

        RaiseChanged();
        return true;
    }

    public bool Pause()
    {
        lock (_sync)
        {
            if (_snapshot.State != UpdateDownloadSessionState.Downloading || _control is null)
            {
                return false;
            }

            _control.Pause();
            _snapshot = _snapshot with
            {
                State = UpdateDownloadSessionState.Paused,
                ContinueInBackground = false
            };
        }

        RaiseChanged();
        return true;
    }

    public bool Resume()
    {
        lock (_sync)
        {
            if (_snapshot.State != UpdateDownloadSessionState.Paused || _control is null)
            {
                return false;
            }

            _control.Resume();
            _snapshot = _snapshot with { State = UpdateDownloadSessionState.Downloading };
        }

        RaiseChanged();
        return true;
    }

    public bool ContinueInBackground()
    {
        lock (_sync)
        {
            if (_snapshot.State is not (UpdateDownloadSessionState.Downloading or UpdateDownloadSessionState.Paused)
                || _control is null)
            {
                return false;
            }

            _control.Resume();
            _snapshot = _snapshot with
            {
                State = UpdateDownloadSessionState.Downloading,
                ContinueInBackground = true
            };
        }

        RaiseChanged();
        return true;
    }

    public bool SetUseAccelerationNodes(bool enabled)
    {
        lock (_sync)
        {
            if (_snapshot.UseAccelerationNodes == enabled)
            {
                return false;
            }

            _snapshot = _snapshot with { UseAccelerationNodes = enabled };
            _control?.SetUseAccelerationNodes(enabled);
        }

        RaiseChanged();
        return true;
    }

    public bool RequestNextAcceleratedNode()
    {
        lock (_sync)
        {
            if (!_snapshot.UseAccelerationNodes || _control is null)
            {
                return false;
            }

            return _control.RequestNextAcceleratedNode();
        }
    }

    public bool Cancel()
    {
        lock (_sync)
        {
            if (_snapshot.State is not (UpdateDownloadSessionState.Downloading or UpdateDownloadSessionState.Paused)
                || _control is null
                || _cancellation is null)
            {
                return false;
            }

            _control.Resume();
            _cancellation.Cancel();
        }

        return true;
    }

    public void PauseWhenUiCloses()
    {
        UpdateDownloadSessionSnapshot snapshot;
        lock (_sync)
        {
            snapshot = _snapshot;
        }

        if (!snapshot.ContinueInBackground)
        {
            Pause();
        }
    }

    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _control?.Resume();
            _cancellation?.Cancel();
            _cancellation?.Dispose();
            _cancellation = null;
            _control = null;
        }
    }

    private async Task RunAsync(
        UpdateRelease release,
        UpdateDownloadControl control,
        CancellationTokenSource cancellation)
    {
        try
        {
            var downloadedPath = await _client.DownloadAndVerifyAsync(
                release,
                new InlineProgress<UpdateDownloadProgress>(ReportProgress),
                control,
                cancellation.Token);
            SetTerminalState(UpdateDownloadSessionState.Completed, downloadedPath, errorMessage: null, cancellation);
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            SetTerminalState(UpdateDownloadSessionState.Cancelled, downloadedPath: null, errorMessage: null, cancellation);
        }
        catch (Exception exception)
        {
            SetTerminalState(UpdateDownloadSessionState.Failed, downloadedPath: null, exception.Message, cancellation);
        }
    }

    private void ReportProgress(UpdateDownloadProgress progress)
    {
        lock (_sync)
        {
            if (_snapshot.State is not (UpdateDownloadSessionState.Downloading or UpdateDownloadSessionState.Paused))
            {
                return;
            }

            _snapshot = _snapshot with { Progress = progress };
        }

        RaiseChanged();
    }

    private void SetTerminalState(
        UpdateDownloadSessionState state,
        string? downloadedPath,
        string? errorMessage,
        CancellationTokenSource cancellation)
    {
        lock (_sync)
        {
            if (!ReferenceEquals(_cancellation, cancellation))
            {
                return;
            }

            _snapshot = _snapshot with
            {
                State = state,
                DownloadedPath = downloadedPath,
                ErrorMessage = errorMessage,
                ContinueInBackground = false
            };
            _control = null;
            _cancellation?.Dispose();
            _cancellation = null;
        }

        RaiseChanged();
    }

    private void RaiseChanged() => Changed?.Invoke(this, Snapshot);

    private void ThrowIfDisposed()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(UpdateDownloadSession));
        }
    }

    private sealed class InlineProgress<T>(Action<T> report) : IProgress<T>
    {
        public void Report(T value) => report(value);
    }
}
