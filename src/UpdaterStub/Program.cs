using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace UpdaterStub;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            var renameTransactionPath = GetArgument(args, "--rename-transaction");
            if (!string.IsNullOrWhiteSpace(renameTransactionPath))
            {
                var renameTransaction = JsonSerializer.Deserialize(
                    File.ReadAllText(renameTransactionPath),
                    UpdaterStubJsonContext.Default.ExecutableRenameTransaction)
                    ?? throw new InvalidDataException("The executable rename transaction is empty.");

                return RunRename(renameTransaction, renameTransactionPath);
            }

            var transactionPath = GetArgument(args, "--transaction")
                ?? throw new ArgumentException("Missing --transaction.");
            var transaction = JsonSerializer.Deserialize(
                File.ReadAllText(transactionPath),
                UpdaterStubJsonContext.Default.UpdateTransaction)
                ?? throw new InvalidDataException("The update transaction is empty.");

            return Run(transaction, transactionPath);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 10;
        }
    }

    private static int RunRename(ExecutableRenameTransaction transaction, string transactionPath)
    {
        if (!WaitForExit(transaction.ParentProcessId, transaction.ParentExitTimeoutSeconds))
        {
            return 20;
        }

        var sourcePath = transaction.SourceExePath;
        var targetPath = transaction.TargetExePath;
        var targetWasPresent = File.Exists(targetPath);

        try
        {
            if (targetWasPresent)
            {
                File.Move(targetPath, transaction.BackupExePath, overwrite: false);
            }

            File.Move(sourcePath, targetPath, overwrite: false);
            var process = StartTarget(targetPath, transaction.HealthMarkerPath);
            if (!WaitForHealth(process, transaction.HealthMarkerPath, transaction.HealthTimeoutSeconds))
            {
                TryTerminate(process);
                RestoreRenamedExecutable(transaction, targetWasPresent);
                TryStartOriginalApplication(sourcePath);
                ScheduleSelfCleanup();
                return 30;
            }

            TryDelete(transaction.BackupExePath);
            TryDeleteDirectory(Path.GetDirectoryName(transactionPath));
            ScheduleSelfCleanup();
            return 0;
        }
        catch
        {
            RestoreRenamedExecutable(transaction, targetWasPresent);
            TryStartOriginalApplication(sourcePath);
            ScheduleSelfCleanup();
            return 40;
        }
    }

    private static int Run(UpdateTransaction transaction, string transactionPath)
    {
        if (!WaitForExit(transaction.ParentProcessId, transaction.ParentExitTimeoutSeconds))
        {
            return 20;
        }

        var targetDirectory = Path.GetDirectoryName(transaction.TargetExePath)
            ?? throw new InvalidOperationException("Target directory is missing.");
        var stagedPath = Path.Combine(targetDirectory, $".{Path.GetFileName(transaction.TargetExePath)}.{Guid.NewGuid():N}.new");

        try
        {
            File.Copy(transaction.DownloadedExePath, stagedPath, overwrite: false);
            File.Move(transaction.TargetExePath, transaction.BackupExePath, overwrite: false);
            File.Move(stagedPath, transaction.TargetExePath, overwrite: false);

            var process = StartTarget(transaction.TargetExePath, transaction.HealthMarkerPath);
            if (!WaitForHealth(process, transaction.HealthMarkerPath, transaction.HealthTimeoutSeconds))
            {
                TryTerminate(process);
                RestoreBackup(transaction);
                TryStartRestoredApplication(transaction.TargetExePath);
                ScheduleSelfCleanup();
                return 30;
            }

            TryDelete(transaction.BackupExePath);
            TryDeleteDirectory(Path.GetDirectoryName(transactionPath));
            ScheduleSelfCleanup();
            return 0;
        }
        catch
        {
            TryDelete(stagedPath);
            RestoreBackup(transaction);
            TryStartRestoredApplication(transaction.TargetExePath);
            ScheduleSelfCleanup();
            return 40;
        }
    }

    private static Process StartTarget(string targetPath, string healthMarkerPath)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = targetPath,
            WorkingDirectory = Path.GetDirectoryName(targetPath) ?? string.Empty,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        startInfo.ArgumentList.Add("--update-health");
        startInfo.ArgumentList.Add(healthMarkerPath);
        return Process.Start(startInfo)
            ?? throw new InvalidOperationException("The updated application could not be started.");
    }

    private static bool WaitForHealth(Process process, string markerPath, int timeoutSeconds)
    {
        var deadline = DateTime.UtcNow.AddSeconds(timeoutSeconds);
        while (DateTime.UtcNow < deadline)
        {
            if (File.Exists(markerPath)) return true;
            if (process.HasExited) return false;
            Thread.Sleep(250);
        }

        return File.Exists(markerPath);
    }

    private static bool WaitForExit(int processId, int timeoutSeconds)
    {
        try
        {
            using var process = Process.GetProcessById(processId);
            return process.WaitForExit(Math.Max(1, timeoutSeconds) * 1000);
        }
        catch (ArgumentException) { return true; }
        catch (InvalidOperationException) { return true; }
    }

    private static void RestoreBackup(UpdateTransaction transaction)
    {
        try
        {
            if (File.Exists(transaction.BackupExePath))
            {
                TryDelete(transaction.TargetExePath);
                File.Move(transaction.BackupExePath, transaction.TargetExePath, overwrite: false);
            }
        }
        catch
        {
            // Keep the backup for manual recovery if restoration itself fails.
        }
    }

    private static void RestoreRenamedExecutable(ExecutableRenameTransaction transaction, bool targetWasPresent)
    {
        try
        {
            if (File.Exists(transaction.TargetExePath))
            {
                if (!File.Exists(transaction.SourceExePath))
                {
                    File.Move(transaction.TargetExePath, transaction.SourceExePath, overwrite: false);
                }
                else
                {
                    TryDelete(transaction.TargetExePath);
                }
            }

            if (targetWasPresent && File.Exists(transaction.BackupExePath))
            {
                File.Move(transaction.BackupExePath, transaction.TargetExePath, overwrite: false);
            }
        }
        catch
        {
            // Keep whichever executable remains for manual recovery.
        }
    }

    private static void TryStartRestoredApplication(string targetPath)
    {
        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = targetPath,
                WorkingDirectory = Path.GetDirectoryName(targetPath) ?? string.Empty,
                UseShellExecute = false,
                CreateNoWindow = true
            });
        }
        catch
        {
            // The backup remains on disk for manual recovery.
        }
    }

    private static void TryStartOriginalApplication(string sourcePath)
    {
        try
        {
            if (File.Exists(sourcePath))
            {
                Process.Start(new ProcessStartInfo
                {
                    FileName = sourcePath,
                    WorkingDirectory = Path.GetDirectoryName(sourcePath) ?? string.Empty,
                    UseShellExecute = false,
                    CreateNoWindow = true
                });
            }
        }
        catch
        {
            // The original executable remains on disk for manual recovery.
        }
    }

    private static void TryTerminate(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit(5000);
            }
        }
        catch
        {
        }
        finally
        {
            process.Dispose();
        }
    }

    private static string? GetArgument(string[] args, string name)
    {
        for (var i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase)) return args[i + 1];
        }

        return null;
    }

    private static void ScheduleSelfCleanup()
    {
        try
        {
            var currentPath = Environment.ProcessPath;
            if (string.IsNullOrWhiteSpace(currentPath)) return;
            var directory = Path.GetDirectoryName(currentPath);
            if (string.IsNullOrWhiteSpace(directory)) return;

            var cleanupScript = Path.Combine(directory, $"cleanup-{Guid.NewGuid():N}.cmd");
            File.WriteAllText(
                cleanupScript,
                $"@echo off\r\ntimeout /t 2 /nobreak >nul\r\ndel /f /q \"{currentPath}\" >nul 2>&1\r\nrmdir /s /q \"{directory}\" >nul 2>&1\r\ndel /f /q \"{cleanupScript}\" >nul 2>&1\r\n");
            Process.Start(new ProcessStartInfo
            {
                FileName = cleanupScript,
                UseShellExecute = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                CreateNoWindow = true
            });
        }
        catch
        {
            // Stale temp files are safe; the next update can clean them.
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
        }
        catch
        {
        }
    }

    private static void TryDeleteDirectory(string? path)
    {
        try
        {
            if (!string.IsNullOrWhiteSpace(path) && Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }
        catch
        {
        }
    }

}

internal sealed record UpdateTransaction(
    int ParentProcessId,
    string TargetExePath,
    string DownloadedExePath,
    string BackupExePath,
    string HealthMarkerPath,
    int ParentExitTimeoutSeconds,
    int HealthTimeoutSeconds);

internal sealed record ExecutableRenameTransaction(
    int ParentProcessId,
    string SourceExePath,
    string TargetExePath,
    string BackupExePath,
    string HealthMarkerPath,
    int ParentExitTimeoutSeconds,
    int HealthTimeoutSeconds);

[JsonSourceGenerationOptions(WriteIndented = false)]
[JsonSerializable(typeof(UpdateTransaction))]
[JsonSerializable(typeof(ExecutableRenameTransaction))]
internal partial class UpdaterStubJsonContext : JsonSerializerContext;
