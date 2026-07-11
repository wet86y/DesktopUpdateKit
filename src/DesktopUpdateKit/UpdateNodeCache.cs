using System.IO;
using System.Text.Json;

namespace DesktopUpdateKit;

internal sealed class UpdateNodeCache
{
    private const int CacheVersion = 1;
    private static readonly TimeSpan ConfigurationLifetime = TimeSpan.FromDays(7);
    private readonly string _path;

    public UpdateNodeCache(string applicationId)
    {
        var safeApplicationId = string.Concat(applicationId.Where(character =>
            char.IsLetterOrDigit(character) || character is '-' or '_'));
        if (string.IsNullOrWhiteSpace(safeApplicationId))
        {
            safeApplicationId = "desktop-update-kit";
        }

        _path = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            safeApplicationId,
            "update-node-cache.json");
    }

    public async Task<UpdateNodeCacheSnapshot?> LoadAsync(CancellationToken cancellationToken)
    {
        try
        {
            if (!File.Exists(_path))
            {
                return null;
            }

            await using var stream = File.OpenRead(_path);
            var cachedSnapshot = await JsonSerializer.DeserializeAsync<UpdateNodeCacheSnapshot>(stream, cancellationToken: cancellationToken);
            if (cachedSnapshot is not { } snapshot || snapshot.Version != CacheVersion)
            {
                return null;
            }

            if (snapshot.NodesUpdatedAtUtc is DateTimeOffset updatedAt
                && DateTimeOffset.UtcNow - updatedAt > ConfigurationLifetime)
            {
                return snapshot with { Nodes = null, NodesUpdatedAtUtc = null };
            }

            return snapshot;
        }
        catch
        {
            return null;
        }
    }

    public async Task SaveNodesAsync(IReadOnlyList<UpdateDownloadNode> nodes, CancellationToken cancellationToken)
    {
        var snapshot = await LoadAsync(cancellationToken) ?? new UpdateNodeCacheSnapshot(CacheVersion);
        await SaveAsync(snapshot with
        {
            Nodes = nodes.ToArray(),
            NodesUpdatedAtUtc = DateTimeOffset.UtcNow
        }, cancellationToken);
    }

    public async Task SaveSuccessAsync(string nodeId, TimeSpan latency, CancellationToken cancellationToken)
    {
        var snapshot = await LoadAsync(cancellationToken) ?? new UpdateNodeCacheSnapshot(CacheVersion);
        await SaveAsync(snapshot with
        {
            LastSuccessNodeId = nodeId,
            LastSuccessLatencyMilliseconds = Math.Max(0, (long)latency.TotalMilliseconds),
            LastSuccessAtUtc = DateTimeOffset.UtcNow
        }, cancellationToken);
    }

    private async Task SaveAsync(UpdateNodeCacheSnapshot snapshot, CancellationToken cancellationToken)
    {
        try
        {
            var directory = Path.GetDirectoryName(_path)!;
            Directory.CreateDirectory(directory);
            var temporaryPath = _path + ".tmp";
            await using (var stream = File.Create(temporaryPath))
            {
                await JsonSerializer.SerializeAsync(stream, snapshot, cancellationToken: cancellationToken);
            }

            File.Move(temporaryPath, _path, overwrite: true);
        }
        catch
        {
            // A cache miss only changes download ordering; it must not stop an update.
        }
    }
}

internal sealed record UpdateNodeCacheSnapshot(
    int Version,
    UpdateDownloadNode[]? Nodes = null,
    DateTimeOffset? NodesUpdatedAtUtc = null,
    string? LastSuccessNodeId = null,
    long? LastSuccessLatencyMilliseconds = null,
    DateTimeOffset? LastSuccessAtUtc = null);
