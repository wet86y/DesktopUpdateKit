using System.Diagnostics;
using System.IO;
using System.Security;
using System.Text.Json;

namespace DesktopUpdateKit;

public sealed class UpdateLauncher
{
    private readonly IUpdaterStubSource _stubSource;

    public UpdateLauncher(IUpdaterStubSource? stubSource = null)
    {
        _stubSource = stubSource ?? new EmbeddedResourceUpdaterStubSource();
    }

    [Obsolete("Pass the verified release SHA-256 so the updater stub can revalidate the staged executable.")]
    public async Task LaunchAsync(string downloadedExePath, CancellationToken cancellationToken = default)
    {
        await LaunchCoreAsync(downloadedExePath, expectedSha256: null, cancellationToken).ConfigureAwait(false);
    }

    public async Task LaunchAsync(
        string downloadedExePath,
        string expectedSha256,
        CancellationToken cancellationToken = default)
    {
        if (!IsSha256(expectedSha256))
        {
            throw new ArgumentException("The expected SHA-256 must contain exactly 64 hexadecimal characters.", nameof(expectedSha256));
        }

        await LaunchCoreAsync(downloadedExePath, expectedSha256.ToLowerInvariant(), cancellationToken).ConfigureAwait(false);
    }

    private async Task LaunchCoreAsync(
        string downloadedExePath,
        string? expectedSha256,
        CancellationToken cancellationToken)
    {
        if (!File.Exists(downloadedExePath))
        {
            throw new FileNotFoundException("The downloaded update does not exist.", downloadedExePath);
        }

        var targetExePath = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(targetExePath)
            || !Path.IsPathFullyQualified(targetExePath)
            || !string.Equals(Path.GetExtension(targetExePath), ".exe", StringComparison.OrdinalIgnoreCase)
            || !File.Exists(targetExePath))
        {
            throw new InvalidOperationException("The current process is not running from a replaceable EXE.");
        }

        var targetDirectory = Path.GetDirectoryName(targetExePath);
        if (string.IsNullOrWhiteSpace(targetDirectory))
        {
            throw new InvalidOperationException("The current EXE directory could not be determined.");
        }

        EnsureDirectoryWritable(targetDirectory);

        var updateDirectory = Path.GetDirectoryName(downloadedExePath);
        if (string.IsNullOrWhiteSpace(updateDirectory))
        {
            throw new InvalidOperationException("The update directory could not be determined.");
        }

        var transactionId = Guid.NewGuid().ToString("N");
        var stubPath = Path.Combine(updateDirectory, "UpdaterStub.exe");
        var transactionPath = Path.Combine(updateDirectory, "update.json");
        var healthMarkerPath = Path.Combine(updateDirectory, "healthy.ok");
        var backupPath = Path.Combine(targetDirectory, $".{Path.GetFileName(targetExePath)}.{transactionId}.bak");

        await ExtractStubAsync(stubPath, cancellationToken).ConfigureAwait(false);
        var transaction = new UpdateTransaction(
            Environment.ProcessId,
            Path.GetFullPath(targetExePath),
            Path.GetFullPath(downloadedExePath),
            backupPath,
            healthMarkerPath,
            ExpectedSha256: expectedSha256);
        await File.WriteAllTextAsync(
            transactionPath,
            JsonSerializer.Serialize(transaction, new JsonSerializerOptions { WriteIndented = true }),
            cancellationToken).ConfigureAwait(false);

        var startInfo = new ProcessStartInfo
        {
            FileName = stubPath,
            WorkingDirectory = updateDirectory,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        startInfo.ArgumentList.Add("--transaction");
        startInfo.ArgumentList.Add(transactionPath);

        if (Process.Start(startInfo) is null)
        {
            throw new InvalidOperationException("UpdaterStub could not be started.");
        }
    }

    public async Task<bool> EnsureCanonicalExecutableNameAsync(
        string canonicalFileName,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(canonicalFileName)
            || !string.Equals(Path.GetExtension(canonicalFileName), ".exe", StringComparison.OrdinalIgnoreCase)
            || canonicalFileName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
        {
            throw new ArgumentException("The canonical executable name is invalid.", nameof(canonicalFileName));
        }

        var currentExePath = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(currentExePath)
            || !Path.IsPathFullyQualified(currentExePath)
            || !File.Exists(currentExePath))
        {
            return false;
        }

        if (string.Equals(Path.GetFileName(currentExePath), canonicalFileName, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var targetDirectory = Path.GetDirectoryName(currentExePath);
        if (string.IsNullOrWhiteSpace(targetDirectory))
        {
            return false;
        }

        var updateDirectory = Path.Combine(
            Path.GetTempPath(),
            "DesktopUpdateKit-name-normalize",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(updateDirectory);
        var stubPath = Path.Combine(updateDirectory, "UpdaterStub.exe");
        var transactionPath = Path.Combine(updateDirectory, "rename.json");
        var healthMarkerPath = Path.Combine(updateDirectory, "healthy.ok");
        var targetExePath = Path.Combine(targetDirectory, canonicalFileName);
        var backupPath = Path.Combine(targetDirectory, $".{canonicalFileName}.{Guid.NewGuid():N}.bak");

        await ExtractStubAsync(stubPath, cancellationToken).ConfigureAwait(false);
        var transaction = new ExecutableRenameTransaction(
            Environment.ProcessId,
            Path.GetFullPath(currentExePath),
            Path.GetFullPath(targetExePath),
            backupPath,
            healthMarkerPath);
        await File.WriteAllTextAsync(
            transactionPath,
            JsonSerializer.Serialize(transaction, new JsonSerializerOptions { WriteIndented = true }),
            cancellationToken).ConfigureAwait(false);

        var startInfo = new ProcessStartInfo
        {
            FileName = stubPath,
            WorkingDirectory = updateDirectory,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        startInfo.ArgumentList.Add("--rename-transaction");
        startInfo.ArgumentList.Add(transactionPath);

        if (Process.Start(startInfo) is null)
        {
            throw new InvalidOperationException("UpdaterStub could not be started for executable name normalization.");
        }

        return true;
    }

    private async Task ExtractStubAsync(string destinationPath, CancellationToken cancellationToken)
    {
        await using var resource = await _stubSource.OpenReadAsync(cancellationToken).ConfigureAwait(false);

        await using var output = new FileStream(destinationPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        await resource.CopyToAsync(output, cancellationToken).ConfigureAwait(false);
    }

    private static bool IsSha256(string? value) =>
        value is { Length: 64 } && value.All(Uri.IsHexDigit);

    private static void EnsureDirectoryWritable(string directory)
    {
        var probePath = Path.Combine(directory, $".update-write-test.{Guid.NewGuid():N}.tmp");
        try
        {
            using (File.Create(probePath))
            {
            }
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or IOException or SecurityException)
        {
            throw new UnauthorizedAccessException(
                "The program directory is not writable. Move the app to a user-writable folder before updating.",
                ex);
        }
        finally
        {
            try
            {
                if (File.Exists(probePath))
                {
                    File.Delete(probePath);
                }
            }
            catch
            {
                // The probe is not part of the update transaction.
            }
        }
    }
}

internal sealed record ExecutableRenameTransaction(
    int ParentProcessId,
    string SourceExePath,
    string TargetExePath,
    string BackupExePath,
    string HealthMarkerPath,
    int ParentExitTimeoutSeconds = 30,
    int HealthTimeoutSeconds = 30);
