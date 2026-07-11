using System.IO;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;

namespace DesktopUpdateKit;

public sealed class UpdateClient
{
    private readonly UpdateClientOptions _options;
    private readonly HttpClient _httpClient;

    public UpdateClient(UpdateClientOptions options, HttpClient? httpClient = null)
    {
        _options = options ?? throw new ArgumentNullException(nameof(options));
        _httpClient = httpClient ?? new HttpClient();
        _httpClient.DefaultRequestHeaders.UserAgent.ParseAdd($"{_options.ApplicationId}-UpdateClient/1.0");
        _httpClient.DefaultRequestHeaders.CacheControl = new System.Net.Http.Headers.CacheControlHeaderValue
        {
            NoCache = true
        };
    }

    public async Task<UpdateRelease?> CheckForUpdateAsync(CancellationToken cancellationToken = default)
    {
        var manifestUrl = $"https://github.com/{_options.Repository}/releases/latest/download/update.json";
        using var response = await _httpClient.GetAsync(manifestUrl, cancellationToken);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        var release = await ParseManifestAsync(stream, cancellationToken);
        return release is not null && release.Version > GetCurrentVersion() ? release : null;
    }

    public async Task<string> DownloadAndVerifyAsync(
        UpdateRelease release,
        IProgress<double>? progress = null,
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
            await DownloadFileAsync(release.ExeDownloadUrl, downloadedPath, progress, cancellationToken);
            await DownloadFileAsync(release.Sha256DownloadUrl, shaPath, null, cancellationToken);

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
        IProgress<double>? progress,
        CancellationToken cancellationToken)
    {
        using var response = await _httpClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        var total = response.Content.Headers.ContentLength;
        await using var input = await response.Content.ReadAsStreamAsync(cancellationToken);
        await using var output = new FileStream(destinationPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);

        var buffer = new byte[128 * 1024];
        long copied = 0;
        int read;
        while ((read = await input.ReadAsync(buffer, cancellationToken)) > 0)
        {
            await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
            copied += read;
            if (total is > 0)
            {
                progress?.Report((double)copied / total.Value);
            }
        }
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
