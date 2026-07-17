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
public sealed class UpdateDownloadSession : IDisposable, IAsyncDisposable
{
    private readonly object _sync = new();
    private readonly object _callbackSync = new();
    private readonly UpdateClient _client;
    private UpdateDownloadControl? _control;
    private CancellationTokenSource? _cancellation;
    private Task? _runTask;
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
            _control = new UpdateDownloadControl(useAccelerationNodes);
            _snapshot = new UpdateDownloadSessionSnapshot(
                UpdateDownloadSessionState.Downloading,
                release,
                Progress: null,
                DownloadedPath: null,
                ErrorMessage: null,
                ContinueInBackground: false,
                UseAccelerationNodes: useAccelerationNodes);
            _runTask = RunAsync(release, _control, _cancellation);
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
            _client.CancelActiveRequests();
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

    /// <summary>Cancels active work and waits up to the supplied timeout for the worker to stop.</summary>
    public async Task<bool> StopAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        if (timeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(timeout));
        }

        Task? worker;
        lock (_sync)
        {
            worker = _runTask;
            _control?.Resume();
            _cancellation?.Cancel();
            _client.CancelActiveRequests();
        }

        if (worker is null || worker.IsCompleted)
        {
            return true;
        }

        try
        {
            await worker.WaitAsync(timeout, cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (TimeoutException)
        {
            return false;
        }
    }

    /// <summary>Deletes a verified download after the host decides not to install it.</summary>
    public bool DiscardCompleted()
    {
        string? downloadedPath;
        lock (_sync)
        {
            ThrowIfDisposed();
            if (_snapshot.State != UpdateDownloadSessionState.Completed
                || string.IsNullOrWhiteSpace(_snapshot.DownloadedPath))
            {
                return false;
            }

            downloadedPath = _snapshot.DownloadedPath;
            _snapshot = new UpdateDownloadSessionSnapshot(
                UpdateDownloadSessionState.Idle, null, null, null, null, false, _snapshot.UseAccelerationNodes);
        }

        try
        {
            var directory = Path.GetDirectoryName(downloadedPath);
            if (!string.IsNullOrWhiteSpace(directory) && Directory.Exists(directory))
            {
                Directory.Delete(directory, recursive: true);
            }
        }
        catch
        {
            // A stale verified download is safe and can be cleaned by a later run.
        }

        RaiseChanged();
        return true;
    }

    public void Dispose()
    {
        lock (_callbackSync)
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
                _client.CancelActiveRequests();
                Changed = null;
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        await StopAsync(TimeSpan.FromSeconds(30)).ConfigureAwait(false);
        Dispose();
        GC.SuppressFinalize(this);
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
            _runTask = null;
        }

        RaiseChanged();
    }

    private void RaiseChanged()
    {
        lock (_callbackSync)
        {
            EventHandler<UpdateDownloadSessionSnapshot>? handler;
            UpdateDownloadSessionSnapshot snapshot;
            lock (_sync)
            {
                if (_disposed)
                {
                    return;
                }

                handler = Changed;
                snapshot = _snapshot;
            }

            handler?.Invoke(this, snapshot);
        }
    }

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
