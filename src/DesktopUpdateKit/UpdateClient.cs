using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace DesktopUpdateKit;

public sealed class UpdateClient
{
    private const int ProbeByteCount = 64 * 1024;
    private static readonly UpdateDownloadNode[] BuiltInNodes =
    [
        new("gh-proxy", "https://gh-proxy.com/{url}", 10),
        new("gh-llkk", "https://gh.llkk.cc/{url}", 20),
        new("ghproxy-net", "https://ghproxy.net/{url}", 30),
        new("github-direct", "{url}", 1000)
    ];

    private readonly UpdateClientOptions _options;
    private readonly HttpClient _httpClient;
    private readonly UpdateDownloadOptions _downloadOptions;
    private readonly UpdateNodeCache _nodeCache;

    public UpdateClient(UpdateClientOptions options, HttpClient? httpClient = null)
    {
        _options = options ?? throw new ArgumentNullException(nameof(options));
        _downloadOptions = _options.DownloadOptions ?? new UpdateDownloadOptions();
        _nodeCache = new UpdateNodeCache(_options.ApplicationId);
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
        if (release is null)
        {
            return null;
        }

        var remoteNodes = NormalizeNodes(release.DownloadNodes);
        if (remoteNodes.Count > 0)
        {
            await _nodeCache.SaveNodesAsync(remoteNodes, cancellationToken);
        }

        return release.Version > GetCurrentVersion() ? release : null;
    }

    public async Task<string> DownloadAndVerifyAsync(
        UpdateRelease release,
        IProgress<UpdateDownloadProgress>? progress = null,
        UpdateDownloadControl? downloadControl = null,
        CancellationToken cancellationToken = default)
    {
        if (release.ExeSize is not > 0)
        {
            throw new InvalidDataException("The update manifest is missing a valid executable size.");
        }

        var expectedHash = ParseSha256(release.ExpectedSha256);
        var tempDirectoryName = string.IsNullOrWhiteSpace(_options.TempDirectoryName)
            ? $"{_options.ApplicationId}-update"
            : _options.TempDirectoryName;
        var updateDirectory = Path.Combine(Path.GetTempPath(), tempDirectoryName, Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(updateDirectory);

        var downloadedPath = Path.Combine(updateDirectory, _options.ExeAssetName);
        var shaPath = Path.Combine(updateDirectory, _options.Sha256AssetName);

        try
        {
            // The manifest SHA-256 and the separately published SHA file must
            // agree before any third-party node is trusted with the EXE bytes.
            await DownloadSingleAsync(
                release.Sha256DownloadUrl,
                shaPath,
                expectedBytes: null,
                progress: null,
                downloadControl: null,
                nodeId: "github-direct",
                cancellationToken);
            var publishedHash = ParseSha256(await File.ReadAllTextAsync(shaPath, cancellationToken));
            if (!CryptographicOperations.FixedTimeEquals(
                    Convert.FromHexString(expectedHash),
                    Convert.FromHexString(publishedHash)))
            {
                throw new InvalidDataException("The update manifest SHA-256 does not match the published SHA-256 asset.");
            }

            var cacheSnapshot = await _nodeCache.LoadAsync(cancellationToken);
            var usingAccelerationNodes = downloadControl?.UseAccelerationNodes ?? true;
            var nodes = ResolveNodes(release.DownloadNodes, cacheSnapshot, usingAccelerationNodes);
            var failures = new List<string>();
            var nodeIndex = 0;
            while (nodeIndex < nodes.Count)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var node = nodes[nodeIndex];
                var downloadUrl = BuildNodeUrl(node, release.ExeDownloadUrl);
                using var nodeAttemptCancellation = downloadControl is null
                    ? null
                    : CancellationTokenSource.CreateLinkedTokenSource(
                        cancellationToken,
                        downloadControl.GetNodeSwitchToken());
                var nodeCancellationToken = nodeAttemptCancellation?.Token ?? cancellationToken;
                var probe = await ProbeNodeAsync(downloadUrl, release.ExeSize.Value, nodeCancellationToken);
                if (probe is null)
                {
                    if (TryApplyNodeSwitchRequest(downloadControl, usingAccelerationNodes, release.DownloadNodes, ref nodes, ref nodeIndex, ref usingAccelerationNodes, cacheSnapshot))
                    {
                        continue;
                    }

                    failures.Add($"{node.Id}: probe failed");
                    nodeIndex++;
                    continue;
                }

                try
                {
                    await DownloadFromNodeWithRetryAsync(
                        downloadUrl,
                        downloadedPath,
                        release.ExeSize.Value,
                        probe.SupportsRanges,
                        node.Id,
                        progress,
                        downloadControl,
                        nodeCancellationToken);

                    var actualHash = await ComputeSha256Async(downloadedPath, cancellationToken);
                    if (!CryptographicOperations.FixedTimeEquals(
                            Convert.FromHexString(expectedHash),
                            Convert.FromHexString(actualHash)))
                    {
                        throw new InvalidDataException("The downloaded update failed SHA-256 verification.");
                    }

                    await _nodeCache.SaveSuccessAsync(node.Id, probe.Latency, cancellationToken);
                    return downloadedPath;
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    throw;
                }
                catch (Exception exception) when (IsNodeFailure(exception))
                {
                    TryDeleteFile(downloadedPath);
                    if (TryApplyNodeSwitchRequest(downloadControl, usingAccelerationNodes, release.DownloadNodes, ref nodes, ref nodeIndex, ref usingAccelerationNodes, cacheSnapshot))
                    {
                        continue;
                    }

                    failures.Add($"{node.Id}: {SummarizeNodeFailure(exception)}");
                    nodeIndex++;
                }
            }

            throw new InvalidOperationException($"All update download nodes failed. {string.Join("; ", failures)}");
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
        var releaseNotes = GetOptionalString(root, "releaseNotes") ?? string.Empty;
        var exeSize = GetOptionalInt64(root, "size");
        var expectedSha256 = GetRequiredString(root, "sha256");
        var nodes = ParseNodes(root);

        // Download nodes must always receive a fixed tag URL rather than a
        // latest-release URL, otherwise a release change during download could
        // produce bytes that do not match this manifest's hash.
        var exeUrl = BuildTaggedAssetUrl(tagName, assetName);
        var shaUrl = BuildTaggedAssetUrl(tagName, sha256AssetName);

        return new UpdateRelease(
            version,
            tagName,
            releasePageUrl,
            releaseNotes,
            exeUrl,
            shaUrl,
            exeSize,
            expectedSha256,
            nodes);
    }

    private string BuildTaggedAssetUrl(string tagName, string assetName) =>
        $"https://github.com/{_options.Repository}/releases/download/{Uri.EscapeDataString(tagName)}/{Uri.EscapeDataString(assetName)}";

    private static Version GetCurrentVersion() => ApplicationVersionProvider.GetCurrentVersion();

    private static IReadOnlyList<UpdateDownloadNode>? ParseNodes(JsonElement root)
    {
        if (!root.TryGetProperty("downloadNodes", out var nodesElement)
            || nodesElement.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        var nodes = new List<UpdateDownloadNode>();
        foreach (var element in nodesElement.EnumerateArray())
        {
            if (element.ValueKind != JsonValueKind.Object)
            {
                continue;
            }

            var id = GetOptionalString(element, "id");
            var template = GetOptionalString(element, "template");
            var priority = GetOptionalInt32(element, "priority");
            var enabled = GetOptionalBoolean(element, "enabled") ?? true;
            if (id is not null && template is not null && priority is not null)
            {
                nodes.Add(new UpdateDownloadNode(id, template, priority.Value, enabled));
            }
        }

        return nodes;
    }

    private static IReadOnlyList<UpdateDownloadNode> NormalizeNodes(IReadOnlyList<UpdateDownloadNode>? nodes)
    {
        if (nodes is null)
        {
            return [];
        }

        var result = new List<UpdateDownloadNode>();
        var knownIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var node in nodes)
        {
            if (!TryNormalizeNode(node, out var normalized)
                || !knownIds.Add(normalized.Id))
            {
                continue;
            }

            result.Add(normalized);
        }

        return result;
    }

    private static bool TryNormalizeNode(UpdateDownloadNode node, out UpdateDownloadNode normalized)
    {
        normalized = default!;
        var id = node.Id?.Trim();
        var template = node.Template?.Trim();
        if (string.IsNullOrWhiteSpace(id)
            || id.Length > 64
            || id.Any(character => !char.IsLetterOrDigit(character) && character is not '-' and not '_')
            || string.IsNullOrWhiteSpace(template)
            || template.CountOccurrences("{url}") != 1)
        {
            return false;
        }

        if (string.Equals(id, "github-direct", StringComparison.OrdinalIgnoreCase))
        {
            normalized = new UpdateDownloadNode("github-direct", "{url}", 1000, true);
            return true;
        }

        var candidate = template.Replace("{url}", "https://github.com/owner/repository/releases/download/v1.0.0/application.exe", StringComparison.Ordinal);
        if (!Uri.TryCreate(candidate, UriKind.Absolute, out var uri)
            || uri.Scheme != Uri.UriSchemeHttps)
        {
            return false;
        }

        normalized = new UpdateDownloadNode(id, template, Math.Clamp(node.Priority, -10_000, 10_000), node.Enabled);
        return true;
    }

    private static IReadOnlyList<UpdateDownloadNode> ResolveNodes(
        IReadOnlyList<UpdateDownloadNode>? remoteNodes,
        UpdateNodeCacheSnapshot? cacheSnapshot,
        bool useAccelerationNodes)
    {
        var directNode = BuiltInNodes.Single(node => node.Id == "github-direct");
        if (!useAccelerationNodes)
        {
            return [directNode];
        }

        var configuredNodes = NormalizeNodes(remoteNodes);
        if (configuredNodes.Count == 0)
        {
            configuredNodes = NormalizeNodes(cacheSnapshot?.Nodes);
        }

        if (configuredNodes.Count == 0)
        {
            configuredNodes = NormalizeNodes(BuiltInNodes);
        }

        var deduplicated = configuredNodes
            .Where(node => !string.Equals(node.Id, directNode.Id, StringComparison.OrdinalIgnoreCase))
            .Append(directNode)
            .Where(node => node.Enabled)
            .OrderBy(node => node.Priority)
            .ToList();

        if (!string.IsNullOrWhiteSpace(cacheSnapshot?.LastSuccessNodeId))
        {
            var successIndex = deduplicated.FindIndex(node => string.Equals(
                node.Id,
                cacheSnapshot.LastSuccessNodeId,
                StringComparison.OrdinalIgnoreCase));
            if (successIndex > 0)
            {
                var lastSuccess = deduplicated[successIndex];
                deduplicated.RemoveAt(successIndex);
                deduplicated.Insert(0, lastSuccess);
            }
        }

        return deduplicated;
    }

    private static bool TryApplyNodeSwitchRequest(
        UpdateDownloadControl? control,
        bool wasUsingAccelerationNodes,
        IReadOnlyList<UpdateDownloadNode>? remoteNodes,
        ref IReadOnlyList<UpdateDownloadNode> nodes,
        ref int nodeIndex,
        ref bool usingAccelerationNodes,
        UpdateNodeCacheSnapshot? cacheSnapshot)
    {
        if (control is null)
        {
            return false;
        }

        var request = control.ConsumeNodeSwitchRequest(wasUsingAccelerationNodes);
        if (request == UpdateNodeSwitchRequest.None)
        {
            return false;
        }

        usingAccelerationNodes = control.UseAccelerationNodes;
        nodes = ResolveNodes(remoteNodes, cacheSnapshot, usingAccelerationNodes);
        nodeIndex = request == UpdateNodeSwitchRequest.NextAcceleratedNode
            ? FindNextAcceleratedNodeIndex(nodes, nodeIndex)
            : 0;
        return nodeIndex >= 0 && nodeIndex < nodes.Count;
    }

    private static int FindNextAcceleratedNodeIndex(IReadOnlyList<UpdateDownloadNode> nodes, int currentIndex)
    {
        for (var offset = 1; offset <= nodes.Count; offset++)
        {
            var index = (currentIndex + offset) % nodes.Count;
            if (!string.Equals(nodes[index].Id, "github-direct", StringComparison.OrdinalIgnoreCase))
            {
                return index;
            }
        }

        return -1;
    }

    private static string BuildNodeUrl(UpdateDownloadNode node, string githubUrl) =>
        node.Template.Replace("{url}", githubUrl, StringComparison.Ordinal);

    private async Task<NodeProbeResult?> ProbeNodeAsync(
        string url,
        long expectedBytes,
        CancellationToken cancellationToken)
    {
        try
        {
            using var requestTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            requestTimeout.CancelAfter(_downloadOptions.EffectiveProbeTimeout);
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, Math.Min(ProbeByteCount - 1, expectedBytes - 1));
            var stopwatch = Stopwatch.StartNew();
            using var response = await _httpClient.SendAsync(
                request,
                HttpCompletionOption.ResponseHeadersRead,
                requestTimeout.Token);

            if (response.StatusCode is not HttpStatusCode.OK and not HttpStatusCode.PartialContent
                || IsHtml(response))
            {
                return null;
            }

            var supportsRanges = response.StatusCode == HttpStatusCode.PartialContent;
            if (supportsRanges)
            {
                var contentRange = response.Content.Headers.ContentRange;
                if (contentRange?.From != 0
                    || contentRange.Length != expectedBytes
                    || contentRange.To != Math.Min(ProbeByteCount - 1, expectedBytes - 1))
                {
                    return null;
                }
            }
            else if (response.Content.Headers.ContentLength is long contentLength && contentLength != expectedBytes)
            {
                return null;
            }

            await using var stream = await response.Content.ReadAsStreamAsync(requestTimeout.Token);
            var header = new byte[2];
            var read = await stream.ReadAsync(header, requestTimeout.Token);
            if (read != header.Length || header[0] != (byte)'M' || header[1] != (byte)'Z')
            {
                return null;
            }

            return new NodeProbeResult(supportsRanges, stopwatch.Elapsed);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return null;
        }
        catch
        {
            return null;
        }
    }

    private async Task DownloadFromNodeWithRetryAsync(
        string url,
        string destinationPath,
        long expectedBytes,
        bool supportsRanges,
        string nodeId,
        IProgress<UpdateDownloadProgress>? progress,
        UpdateDownloadControl? downloadControl,
        CancellationToken cancellationToken)
    {
        if (supportsRanges && expectedBytes >= _downloadOptions.EffectiveMinimumParallelDownloadBytes)
        {
            try
            {
                await DownloadInParallelAsync(
                    url,
                    destinationPath,
                    expectedBytes,
                    nodeId,
                    progress,
                    downloadControl,
                    cancellationToken);
                return;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception) when (IsNodeFailure(exception))
            {
                // A successful 64 KiB Range probe does not prove that a public
                // node can sustain several concurrent ranges. Any parallel
                // transport failure therefore degrades this same node to a
                // single connection before another node is considered.
                TryDeleteFile(destinationPath);
            }
        }

        await DownloadSingleWithRetryAsync(
            url,
            destinationPath,
            expectedBytes,
            nodeId,
            progress,
            downloadControl,
            isParallelFallback: supportsRanges,
            cancellationToken);
    }

    private async Task DownloadSingleWithRetryAsync(
        string url,
        string destinationPath,
        long expectedBytes,
        string nodeId,
        IProgress<UpdateDownloadProgress>? progress,
        UpdateDownloadControl? downloadControl,
        bool isParallelFallback,
        CancellationToken cancellationToken)
    {
        Exception? lastException = null;
        for (var attempt = 0; attempt <= _downloadOptions.EffectiveMaxRetriesPerNode; attempt++)
        {
            TryDeleteFile(destinationPath);
            try
            {
                await DownloadSingleAsync(
                    url,
                    destinationPath,
                    expectedBytes,
                    progress,
                    downloadControl,
                    nodeId,
                    cancellationToken,
                    isParallelFallback);
                return;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception) when (IsNodeFailure(exception))
            {
                lastException = exception;
            }
        }

        throw new NodeDownloadException("The node did not complete the update download after retrying once.", lastException);
    }

    private async Task DownloadInParallelAsync(
        string url,
        string destinationPath,
        long totalBytes,
        string nodeId,
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

        var reporter = new DownloadProgressReporter(progress, totalBytes, nodeId, ranges.Count);
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
        using var response = await SendWithConnectTimeoutAsync(request, cancellationToken);
        if (response.StatusCode != HttpStatusCode.PartialContent || IsHtml(response))
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
            var read = await ReadWithIdleTimeoutAsync(input, buffer.AsMemory(0, requested), cancellationToken);
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
        long? expectedBytes,
        IProgress<UpdateDownloadProgress>? progress,
        UpdateDownloadControl? downloadControl,
        string nodeId,
        CancellationToken cancellationToken,
        bool isParallelFallback = false)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        using var response = await SendWithConnectTimeoutAsync(request, cancellationToken);
        response.EnsureSuccessStatusCode();
        if (IsHtml(response))
        {
            throw new NodeDownloadException("The node returned an HTML document instead of an executable.");
        }

        var total = response.Content.Headers.ContentLength;
        if (expectedBytes is long expected && total is long declared && declared != expected)
        {
            throw new InvalidDataException("The download node reported an unexpected file size.");
        }

        await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);
        await using var output = new FileStream(destinationPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        var buffer = new byte[_downloadOptions.EffectiveBufferBytes];
        var reporter = new DownloadProgressReporter(
            progress,
            expectedBytes ?? total,
            nodeId,
            connectionCount: 1,
            isParallelFallback);
        long copied = 0;
        var validatedHeader = false;
        while (true)
        {
            if (downloadControl is not null)
            {
                await downloadControl.WaitIfPausedAsync(cancellationToken);
            }

            var read = await ReadWithIdleTimeoutAsync(input, buffer, cancellationToken);
            if (read == 0)
            {
                break;
            }

            if (expectedBytes is not null && !validatedHeader)
            {
                if (read < 2 || buffer[0] != (byte)'M' || buffer[1] != (byte)'Z')
                {
                    throw new NodeDownloadException("The node returned data that is not a Windows executable.");
                }

                validatedHeader = true;
            }

            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
            copied += read;
            reporter.Add(read);
        }

        if (expectedBytes is long expectedBytesValue && copied != expectedBytesValue)
        {
            throw new InvalidDataException("The downloaded update size does not match the manifest.");
        }

        reporter.Complete();
    }

    private async Task<HttpResponseMessage> SendWithConnectTimeoutAsync(
        HttpRequestMessage request,
        CancellationToken cancellationToken)
    {
        using var requestTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        requestTimeout.CancelAfter(_downloadOptions.EffectiveProbeTimeout);
        return await _httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, requestTimeout.Token);
    }

    private async Task<int> ReadWithIdleTimeoutAsync(Stream input, Memory<byte> buffer, CancellationToken cancellationToken)
    {
        using var idleTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        idleTimeout.CancelAfter(_downloadOptions.EffectiveIdleTimeout);
        return await input.ReadAsync(buffer, idleTimeout.Token);
    }

    private static bool IsHtml(HttpResponseMessage response) =>
        response.Content.Headers.ContentType?.MediaType?.Contains("html", StringComparison.OrdinalIgnoreCase) == true;

    private static bool IsNodeFailure(Exception exception) => exception is
        HttpRequestException or
        IOException or
        EndOfStreamException or
        InvalidDataException or
        NodeDownloadException or
        RangeDownloadNotSupportedException or
        OperationCanceledException;

    private static string SummarizeNodeFailure(Exception exception) => exception switch
    {
        OperationCanceledException => "timeout",
        HttpRequestException => "HTTP request failed",
        InvalidDataException => "integrity check failed",
        _ => "download failed"
    };

    private static string ParseSha256(string text)
    {
        var hashToken = text
            .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault(part => part.Length == 64 && part.All(Uri.IsHexDigit));

        if (hashToken is null)
        {
            throw new InvalidDataException("The SHA-256 value has an invalid format.");
        }

        return hashToken.ToUpperInvariant();
    }

    private static async Task<string> ComputeSha256Async(string path, CancellationToken cancellationToken)
    {
        await using var stream = File.OpenRead(path);
        var hash = await SHA256.HashDataAsync(stream, cancellationToken);
        return Convert.ToHexString(hash);
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

    private static string? GetOptionalString(JsonElement root, string propertyName) =>
        root.TryGetProperty(propertyName, out var property)
        && property.ValueKind == JsonValueKind.String
            ? property.GetString()?.Trim()
            : null;

    private static long? GetOptionalInt64(JsonElement root, string propertyName) =>
        root.TryGetProperty(propertyName, out var property)
        && property.TryGetInt64(out var value)
            ? value
            : null;

    private static int? GetOptionalInt32(JsonElement root, string propertyName) =>
        root.TryGetProperty(propertyName, out var property)
        && property.TryGetInt32(out var value)
            ? value
            : null;

    private static bool? GetOptionalBoolean(JsonElement root, string propertyName) =>
        root.TryGetProperty(propertyName, out var property)
        && (property.ValueKind is JsonValueKind.True or JsonValueKind.False)
            ? property.GetBoolean()
            : null;

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

    private sealed record NodeProbeResult(bool SupportsRanges, TimeSpan Latency);

    private sealed record ByteRange(long Start, long End);

    private sealed class RangeDownloadNotSupportedException : Exception
    {
    }

    private sealed class NodeDownloadException : Exception
    {
        public NodeDownloadException(string message, Exception? innerException = null)
            : base(message, innerException)
        {
        }
    }

    private sealed class DownloadProgressReporter
    {
        private readonly IProgress<UpdateDownloadProgress>? _progress;
        private readonly long? _totalBytes;
        private readonly string _nodeId;
        private readonly int _connectionCount;
        private readonly bool _isParallelFallback;
        private readonly Stopwatch _stopwatch = Stopwatch.StartNew();
        private readonly object _sync = new();
        private readonly Queue<(TimeSpan Timestamp, int Bytes)> _speedSamples = new();
        private long _bytesReceived;
        private long _speedWindowBytes;
        private TimeSpan _lastReport;

        public DownloadProgressReporter(
            IProgress<UpdateDownloadProgress>? progress,
            long? totalBytes,
            string nodeId,
            int connectionCount,
            bool isParallelFallback = false)
        {
            _progress = progress;
            _totalBytes = totalBytes;
            _nodeId = nodeId;
            _connectionCount = Math.Max(1, connectionCount);
            _isParallelFallback = isParallelFallback;
        }

        public void Add(int byteCount)
        {
            Interlocked.Add(ref _bytesReceived, byteCount);
            var timestamp = _stopwatch.Elapsed;
            lock (_sync)
            {
                _speedSamples.Enqueue((timestamp, byteCount));
                _speedWindowBytes += byteCount;
                TrimSpeedSamples(timestamp);
            }

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
            double bytesPerSecond;
            lock (_sync)
            {
                if (!force && elapsed - _lastReport < TimeSpan.FromMilliseconds(120))
                {
                    return;
                }

                _lastReport = elapsed;
                TrimSpeedSamples(elapsed);
                var windowStart = _speedSamples.Count > 0 ? _speedSamples.Peek().Timestamp : elapsed;
                var windowSeconds = Math.Max(0.25, (elapsed - windowStart).TotalSeconds);
                bytesPerSecond = _speedWindowBytes / windowSeconds;
            }

            var bytesReceived = Interlocked.Read(ref _bytesReceived);
            _progress.Report(new UpdateDownloadProgress(
                bytesReceived,
                _totalBytes,
                bytesPerSecond,
                _nodeId,
                _connectionCount,
                _isParallelFallback));
        }

        private void TrimSpeedSamples(TimeSpan now)
        {
            while (_speedSamples.Count > 1
                   && now - _speedSamples.Peek().Timestamp > TimeSpan.FromSeconds(2))
            {
                _speedWindowBytes -= _speedSamples.Dequeue().Bytes;
            }
        }
    }
}

internal static class StringExtensions
{
    public static int CountOccurrences(this string value, string token)
    {
        var count = 0;
        var startIndex = 0;
        while ((startIndex = value.IndexOf(token, startIndex, StringComparison.Ordinal)) >= 0)
        {
            count++;
            startIndex += token.Length;
        }

        return count;
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
