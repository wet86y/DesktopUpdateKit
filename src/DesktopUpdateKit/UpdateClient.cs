using System.IO;
using System.Diagnostics;
using System.Net.Http;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace DesktopUpdateKit;

public sealed class UpdateClient
{
    private readonly UpdateClientOptions _options;
    private readonly HttpClient _httpClient;
    private readonly UpdateDownloadOptions _downloadOptions;

    public UpdateClient(UpdateClientOptions options, HttpClient? httpClient = null)
    {
        _options = options ?? throw new ArgumentNullException(nameof(options));
        _downloadOptions = _options.DownloadOptions ?? new UpdateDownloadOptions();
        _httpClient = httpClient ?? new HttpClient();
        _httpClient.DefaultRequestHeaders.UserAgent.ParseAdd($"{_options.ApplicationId}-UpdateClient/1.0");
        _httpClient.DefaultRequestHeaders.CacheControl = new System.Net.Http.Headers.CacheControlHeaderValue
        {
            NoCache = true
        };
        _httpClient.Timeout = Timeout.InfiniteTimeSpan;
    }

    public async Task<UpdateRelease?> CheckForUpdateAsync(CancellationToken cancellationToken = default)
    {
        var manifestUrl = $"https://github.com/{_options.Repository}/releases/latest/download/update.json";
        using var requestTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        requestTimeout.CancelAfter(TimeSpan.FromSeconds(20));
        using var response = await _httpClient.GetAsync(manifestUrl, requestTimeout.Token);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        var release = await ParseManifestAsync(stream, cancellationToken);
        return release is not null && release.Version > GetCurrentVersion() ? release : null;
    }

    public async Task<string> DownloadAndVerifyAsync(
        UpdateRelease release,
        IProgress<UpdateDownloadProgress>? progress = null,
        UpdateDownloadControl? downloadControl = null,
        CancellationToken cancellationToken = default)
    {
        var tempDirectoryName = string.IsNullOrWhiteSpace(_options.TempDirectoryName)
            ? $"{_options.ApplicationId}-update"
            : _options.TempDirectoryName;
        var updateDirectory = Path.Combine(Path.GetTempPath(), tempDirectoryName, Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(updateDirectory);

        var downloadedPath = Path.Combine(updateDirectory, _options.ExeAssetName);
        var shaPath = Path.Combine(updateDirectory, _options.Sha256AssetName);

        try
        {
            await DownloadFileAsync(
                release.ExeDownloadUrl,
                downloadedPath,
                progress,
                downloadControl,
                allowParallelDownload: true,
                cancellationToken);
            await DownloadFileAsync(
                release.Sha256DownloadUrl,
                shaPath,
                progress: null,
                downloadControl: null,
                allowParallelDownload: false,
                cancellationToken);

            var expectedHash = ParseSha256(await File.ReadAllTextAsync(shaPath, cancellationToken));
            var actualHash = await ComputeSha256Async(downloadedPath, cancellationToken);
            if (!CryptographicOperations.FixedTimeEquals(
                    Convert.FromHexString(expectedHash),
                    Convert.FromHexString(actualHash)))
            {
                throw new InvalidDataException("The downloaded update failed SHA-256 verification.");
            }

            return downloadedPath;
        }
        catch
        {
            TryDeleteDirectory(updateDirectory);
            throw;
        }
    }

    private async Task<UpdateRelease?> ParseManifestAsync(Stream stream, CancellationToken cancellationToken)
    {
        using var document = await JsonDocument.ParseAsync(stream, cancellationToken: cancellationToken);
        var root = document.RootElement;
        var versionText = GetRequiredString(root, "version").TrimStart('v', 'V');
        if (!Version.TryParse(versionText, out var version))
        {
            return null;
        }

        var assetName = GetOptionalString(root, "asset") ?? _options.ExeAssetName;
        var sha256AssetName = GetOptionalString(root, "sha256Asset") ?? $"{assetName}.sha256";
        var tagName = GetOptionalString(root, "tag") ?? $"v{version}";
        var releasePageUrl = GetOptionalString(root, "releaseNotesUrl")
            ?? $"https://github.com/{_options.Repository}/releases/tag/{Uri.EscapeDataString(tagName)}";

        // Do not trust arbitrary URLs from the remote manifest. Construct the
        // download endpoints from the configured repository and asset names.
        var exeUrl = BuildLatestAssetUrl(assetName);
        var shaUrl = BuildLatestAssetUrl(sha256AssetName);
        var releaseNotes = GetOptionalString(root, "releaseNotes") ?? string.Empty;
        var exeSize = GetOptionalInt64(root, "size");

        return new UpdateRelease(
            version,
            tagName,
            releasePageUrl,
            releaseNotes,
            exeUrl,
            shaUrl,
            exeSize);
    }

    private string BuildLatestAssetUrl(string assetName) =>
        $"https://github.com/{_options.Repository}/releases/latest/download/{Uri.EscapeDataString(assetName)}";

    private static Version GetCurrentVersion()
    {
        return ApplicationVersionProvider.GetCurrentVersion();
    }

    private static string GetRequiredString(JsonElement root, string propertyName)
    {
        var value = GetOptionalString(root, propertyName);
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new InvalidDataException($"Update manifest is missing {propertyName}.");
        }

        return value;
    }

    private static string? GetOptionalString(JsonElement root, string propertyName)
    {
        return root.TryGetProperty(propertyName, out var property)
            && property.ValueKind == JsonValueKind.String
            ? property.GetString()?.Trim()
            : null;
    }

    private static long? GetOptionalInt64(JsonElement root, string propertyName)
    {
        return root.TryGetProperty(propertyName, out var property)
            && property.TryGetInt64(out var value)
            ? value
            : null;
    }

    private async Task DownloadFileAsync(
        string url,
        string destinationPath,
        IProgress<UpdateDownloadProgress>? progress,
        UpdateDownloadControl? downloadControl,
        bool allowParallelDownload,
        CancellationToken cancellationToken)
    {
        if (allowParallelDownload && _downloadOptions.EffectiveMaxConcurrentConnections > 1)
        {
            var remoteLength = await TryGetRangeLengthAsync(url, cancellationToken);
            if (remoteLength >= _downloadOptions.EffectiveMinimumParallelDownloadBytes)
            {
                try
                {
                    await DownloadInParallelAsync(url, destinationPath, remoteLength.Value, progress, downloadControl, cancellationToken);
                    return;
                }
                catch (RangeDownloadNotSupportedException)
                {
                    TryDeleteFile(destinationPath);
                }
            }
        }

        await DownloadSingleAsync(url, destinationPath, progress, downloadControl, cancellationToken);
    }

    private async Task<long?> TryGetRangeLengthAsync(string url, CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, 0);
        using var response = await _httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();

        return response.StatusCode == HttpStatusCode.PartialContent
            ? response.Content.Headers.ContentRange?.Length
            : null;
    }

    private async Task DownloadInParallelAsync(
        string url,
        string destinationPath,
        long totalBytes,
        IProgress<UpdateDownloadProgress>? progress,
        UpdateDownloadControl? downloadControl,
        CancellationToken cancellationToken)
    {
        var ranges = CreateRanges(totalBytes, _downloadOptions.EffectiveMaxConcurrentConnections);
        await using var output = new FileStream(
            destinationPath,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.Read,
            bufferSize: 1,
            FileOptions.Asynchronous | FileOptions.RandomAccess);
        output.SetLength(totalBytes);

        var reporter = new DownloadProgressReporter(progress, totalBytes);
        SafeFileHandle outputHandle = output.SafeFileHandle;
        var downloads = ranges.Select(range => DownloadRangeAsync(
            url,
            range,
            totalBytes,
            outputHandle,
            reporter,
            downloadControl,
            cancellationToken));

        await Task.WhenAll(downloads);
        await output.FlushAsync(cancellationToken);
        reporter.Complete();
    }

    private async Task DownloadRangeAsync(
        string url,
        ByteRange range,
        long totalBytes,
        SafeFileHandle outputHandle,
        DownloadProgressReporter reporter,
        UpdateDownloadControl? downloadControl,
        CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(range.Start, range.End);
        using var response = await _httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        if (response.StatusCode != HttpStatusCode.PartialContent)
        {
            throw new RangeDownloadNotSupportedException();
        }

        var contentRange = response.Content.Headers.ContentRange;
        if (contentRange?.From != range.Start || contentRange.To != range.End || contentRange.Length != totalBytes)
        {
            throw new RangeDownloadNotSupportedException();
        }

        await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);
        var buffer = new byte[_downloadOptions.EffectiveBufferBytes];
        var offset = range.Start;
        while (offset <= range.End)
        {
            if (downloadControl is not null)
            {
                await downloadControl.WaitIfPausedAsync(cancellationToken);
            }

            var remaining = range.End - offset + 1;
            var requested = (int)Math.Min(buffer.Length, remaining);
            var read = await input.ReadAsync(buffer.AsMemory(0, requested), cancellationToken);
            if (read == 0)
            {
                throw new EndOfStreamException("The update server returned an incomplete byte range.");
            }

            await RandomAccess.WriteAsync(outputHandle, buffer.AsMemory(0, read), offset, cancellationToken);
            offset += read;
            reporter.Add(read);
        }
    }

    private async Task DownloadSingleAsync(
        string url,
        string destinationPath,
        IProgress<UpdateDownloadProgress>? progress,
        UpdateDownloadControl? downloadControl,
        CancellationToken cancellationToken)
    {
        using var response = await _httpClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        var total = response.Content.Headers.ContentLength;
        await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);
        await using var output = new FileStream(destinationPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);

        var buffer = new byte[_downloadOptions.EffectiveBufferBytes];
        var reporter = new DownloadProgressReporter(progress, total);
        int read;
        while (true)
        {
            if (downloadControl is not null)
            {
                await downloadControl.WaitIfPausedAsync(cancellationToken);
            }

            read = await input.ReadAsync(buffer, cancellationToken);
            if (read == 0)
            {
                break;
            }

            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
            reporter.Add(read);
        }

        reporter.Complete();
    }

    private static string ParseSha256(string text)
    {
        var hashToken = text
            .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault(part => part.Length == 64 && part.All(Uri.IsHexDigit));

        if (hashToken is null)
        {
            throw new InvalidDataException("The SHA-256 asset has an invalid format.");
        }

        return hashToken.ToUpperInvariant();
    }

    private static async Task<string> ComputeSha256Async(string path, CancellationToken cancellationToken)
    {
        await using var stream = File.OpenRead(path);
        var hash = await SHA256.HashDataAsync(stream, cancellationToken);
        return Convert.ToHexString(hash);
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }
        catch
        {
            // Best-effort cleanup. The next update can remove stale folders.
        }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
            // The outer transaction cleanup will make a second best-effort attempt.
        }
    }

    private static IReadOnlyList<ByteRange> CreateRanges(long totalBytes, int requestedConnections)
    {
        var count = (int)Math.Min(requestedConnections, totalBytes);
        var ranges = new List<ByteRange>(count);
        var baseLength = totalBytes / count;
        var remainder = totalBytes % count;
        long start = 0;
        for (var index = 0; index < count; index++)
        {
            var length = baseLength + (index < remainder ? 1 : 0);
            ranges.Add(new ByteRange(start, start + length - 1));
            start += length;
        }

        return ranges;
    }

    private sealed record ByteRange(long Start, long End);

    private sealed class RangeDownloadNotSupportedException : Exception
    {
    }

    private sealed class DownloadProgressReporter
    {
        private readonly IProgress<UpdateDownloadProgress>? _progress;
        private readonly long? _totalBytes;
        private readonly Stopwatch _stopwatch = Stopwatch.StartNew();
        private readonly object _sync = new();
        private long _bytesReceived;
        private TimeSpan _lastReport;

        public DownloadProgressReporter(IProgress<UpdateDownloadProgress>? progress, long? totalBytes)
        {
            _progress = progress;
            _totalBytes = totalBytes;
        }

        public void Add(int byteCount)
        {
            Interlocked.Add(ref _bytesReceived, byteCount);
            Report(force: false);
        }

        public void Complete() => Report(force: true);

        private void Report(bool force)
        {
            if (_progress is null)
            {
                return;
            }

            var elapsed = _stopwatch.Elapsed;
            lock (_sync)
            {
                if (!force && elapsed - _lastReport < TimeSpan.FromMilliseconds(120))
                {
                    return;
                }

                _lastReport = elapsed;
            }

            var bytesReceived = Interlocked.Read(ref _bytesReceived);
            var bytesPerSecond = bytesReceived / Math.Max(0.001, elapsed.TotalSeconds);
            _progress.Report(new UpdateDownloadProgress(bytesReceived, _totalBytes, bytesPerSecond));
        }
    }
}

internal static class ApplicationVersionProvider
{
    public static Version GetCurrentVersion()
    {
        var assembly = System.Reflection.Assembly.GetEntryAssembly();
        var versionText = assembly?.GetName().Version?.ToString() ?? "0.0.0";
        return Version.TryParse(versionText, out var version) ? version : new Version(0, 0, 0);
    }
}
