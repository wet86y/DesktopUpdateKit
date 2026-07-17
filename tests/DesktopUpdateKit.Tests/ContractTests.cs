using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace DesktopUpdateKit.Tests;

public sealed class ContractTests
{
    [Fact]
    public async Task ExplicitCurrentVersionControlsUpdateAvailability()
    {
        var manifest = await ReadFixtureAsync("valid-update.json");
        using var currentHttp = new HttpClient(new FixtureHandler(manifest, [(byte)'M', (byte)'Z']));
        var current = new UpdateClient(Options(new Version(9, 0, 0)), currentHttp);
        Assert.Null(await current.CheckForUpdateAsync());

        using var olderHttp = new HttpClient(new FixtureHandler(manifest, [(byte)'M', (byte)'Z']));
        var older = new UpdateClient(Options(new Version(1, 0, 0)), olderHttp);
        Assert.NotNull(await older.CheckForUpdateAsync());
    }

    [Fact]
    public async Task ManifestCannotRedirectConfiguredRepository()
    {
        var manifest = await ReadFixtureAsync("invalid-repository-update.json");
        using var http = new HttpClient(new FixtureHandler(manifest, [0x2A]));
        var client = new UpdateClient(Options(new Version(1, 0, 0)), http);
        await Assert.ThrowsAsync<InvalidDataException>(() => client.CheckForUpdateAsync());
    }

    [Fact]
    public async Task MemoryStubSourceReturnsIndependentReadableStreams()
    {
        var source = new MemoryUpdaterStubSource(new byte[] { 1, 2, 3 });
        await using var first = await source.OpenReadAsync();
        await using var second = await source.OpenReadAsync();
        Assert.Equal(new byte[] { 1, 2, 3 }, await ReadAllAsync(first));
        Assert.Equal(new byte[] { 1, 2, 3 }, await ReadAllAsync(second));
    }

    [Fact]
    public async Task CompletedSessionCanDiscardItsVerifiedDownload()
    {
        var payload = new byte[] { (byte)'M', (byte)'Z', 0x2A, 0x2B };
        var hash = Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();
        var manifest = (await ReadFixtureAsync("valid-update.json"))
            .Replace("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hash, StringComparison.Ordinal)
            .Replace("\"size\": 1", $"\"size\": {payload.Length}", StringComparison.Ordinal);
        var handler = new FixtureHandler(manifest, payload);
        using var http = new HttpClient(handler);
        var client = new UpdateClient(Options(new Version(1, 0, 0)), http);
        var release = Assert.IsType<UpdateRelease>(await client.CheckForUpdateAsync());
        await using var session = new UpdateDownloadSession(client);
        Assert.True(session.TryStart(release, useAccelerationNodes: false));

        try
        {
            await WaitForStateAsync(session, UpdateDownloadSessionState.Completed);
        }
        catch (Exception error)
        {
            throw new Xunit.Sdk.XunitException($"{error.Message}; requests: {string.Join(" | ", handler.Requests)}");
        }
        var downloaded = Assert.IsType<string>(session.Snapshot.DownloadedPath);
        Assert.True(File.Exists(downloaded));
        Assert.True(session.DiscardCompleted());
        Assert.False(File.Exists(downloaded));
        Assert.Equal(UpdateDownloadSessionState.Idle, session.Snapshot.State);
    }

    [Fact]
    public async Task ActiveSessionStopsWithinCallerDeadline()
    {
        var payload = new byte[] { (byte)'M', (byte)'Z', 0x2A, 0x2B };
        var hash = Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();
        var manifest = (await ReadFixtureAsync("valid-update.json"))
            .Replace("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hash, StringComparison.Ordinal)
            .Replace("\"size\": 1", $"\"size\": {payload.Length}", StringComparison.Ordinal);
        var handler = new FixtureHandler(manifest, payload, blockExecutable: true);
        using var http = new HttpClient(handler);
        var client = new UpdateClient(Options(new Version(1, 0, 0)), http);
        var release = Assert.IsType<UpdateRelease>(await client.CheckForUpdateAsync());
        await using var session = new UpdateDownloadSession(client);
        Assert.True(session.TryStart(release, useAccelerationNodes: false));

        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline && !handler.Requests.Any(item => item.Contains("fixture.exe", StringComparison.Ordinal)))
            await Task.Delay(20);

        Assert.True(await session.StopAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(UpdateDownloadSessionState.Cancelled, session.Snapshot.State);
    }

    private static UpdateClientOptions Options(Version currentVersion) => new(
        "fixture-app", "fixture/repo", "fixture.exe", "fixture.exe.sha256",
        TempDirectoryName: $"desktop-update-kit-tests-{Guid.NewGuid():N}",
        CurrentVersion: currentVersion);

    private static async Task<string> ReadFixtureAsync(string name) =>
        await File.ReadAllTextAsync(Path.Combine(AppContext.BaseDirectory, "contracts", name));

    private static async Task<byte[]> ReadAllAsync(Stream stream)
    {
        using var buffer = new MemoryStream();
        await stream.CopyToAsync(buffer);
        return buffer.ToArray();
    }

    private static async Task WaitForStateAsync(
        UpdateDownloadSession session,
        UpdateDownloadSessionState expected)
    {
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < deadline)
        {
            var state = session.Snapshot.State;
            if (state == expected) return;
            if (state is UpdateDownloadSessionState.Failed or UpdateDownloadSessionState.Cancelled)
                throw new Xunit.Sdk.XunitException($"Session ended in {state}: {session.Snapshot.ErrorMessage}");
            await Task.Delay(20);
        }

        throw new TimeoutException($"Session did not reach {expected}.");
    }

    private sealed class FixtureHandler(string manifest, byte[] payload, bool blockExecutable = false) : HttpMessageHandler
    {
        private readonly string _manifest = manifest;
        private readonly byte[] _payload = payload;
        private readonly string _hash = Convert.ToHexString(SHA256.HashData(payload)).ToLowerInvariant();
        private readonly bool _blockExecutable = blockExecutable;
        public List<string> Requests { get; } = [];

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (Requests)
            {
                Requests.Add($"{request.RequestUri} range={request.Headers.Range}");
            }
            var path = request.RequestUri?.AbsolutePath ?? string.Empty;
            if (path.EndsWith("/update.json", StringComparison.Ordinal))
                return Task.FromResult(Response(HttpStatusCode.OK, Encoding.UTF8.GetBytes(_manifest), "application/json"));
            if (path.EndsWith(".sha256", StringComparison.Ordinal))
                return Task.FromResult(Response(HttpStatusCode.OK, Encoding.ASCII.GetBytes($"{_hash}  fixture.exe\n"), "text/plain"));
            if (path.EndsWith("/fixture.exe", StringComparison.Ordinal))
            {
                if (_blockExecutable)
                    return WaitForCancellationAsync(cancellationToken);

                if (request.Headers.Range?.Ranges.SingleOrDefault() is { } range)
                {
                    _ = range;
                    return Task.FromResult(Response(HttpStatusCode.OK, _payload, "application/octet-stream"));
                }

                return Task.FromResult(Response(HttpStatusCode.OK, _payload, "application/octet-stream"));
            }

            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.NotFound));
        }

        private static async Task<HttpResponseMessage> WaitForCancellationAsync(CancellationToken cancellationToken)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            throw new InvalidOperationException("The blocking fixture must be cancelled.");
        }

        private static HttpResponseMessage Response(HttpStatusCode status, byte[] body, string contentType)
        {
            var response = new HttpResponseMessage(status) { Content = new ByteArrayContent(body) };
            response.Content.Headers.ContentType = new(contentType);
            response.Content.Headers.ContentLength = body.Length;
            return response;
        }
    }
}
