using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security;
using System.Text.Json;

namespace DesktopUpdateKit;

public sealed class UpdateLauncher
{
    private const string StubResourceName = "DesktopUpdateKit.Resources.UpdaterStub.exe";

    public async Task LaunchAsync(string downloadedExePath, CancellationToken cancellationToken = default)
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

        await ExtractStubAsync(stubPath, cancellationToken);
        var transaction = new UpdateTransaction(
            Environment.ProcessId,
            Path.GetFullPath(targetExePath),
            Path.GetFullPath(downloadedExePath),
            backupPath,
            healthMarkerPath);
        await File.WriteAllTextAsync(
            transactionPath,
            JsonSerializer.Serialize(transaction, new JsonSerializerOptions { WriteIndented = true }),
            cancellationToken);

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

    private static async Task ExtractStubAsync(string destinationPath, CancellationToken cancellationToken)
    {
        var assembly = Assembly.GetEntryAssembly()
            ?? throw new InvalidOperationException("The application assembly is unavailable.");
        await using var resource = assembly.GetManifestResourceStream(StubResourceName)
            ?? throw new InvalidOperationException(
                "UpdaterStub is not embedded in this build. Use the shared release publishing script first.");

        await using var output = new FileStream(destinationPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        await resource.CopyToAsync(output, cancellationToken);
    }

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
