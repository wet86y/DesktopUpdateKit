using System.Reflection;

namespace DesktopUpdateKit;

/// <summary>Provides the updater stub bytes embedded or supplied by a host application.</summary>
public interface IUpdaterStubSource
{
    ValueTask<Stream> OpenReadAsync(CancellationToken cancellationToken = default);
}

/// <summary>Reads an updater stub from an assembly manifest resource.</summary>
public sealed class EmbeddedResourceUpdaterStubSource : IUpdaterStubSource
{
    public const string DefaultResourceName = "DesktopUpdateKit.Resources.UpdaterStub.exe";

    private readonly Assembly? _assembly;
    private readonly string _resourceName;

    public EmbeddedResourceUpdaterStubSource(
        Assembly? assembly = null,
        string resourceName = DefaultResourceName)
    {
        if (string.IsNullOrWhiteSpace(resourceName))
        {
            throw new ArgumentException("The updater stub resource name is required.", nameof(resourceName));
        }

        _assembly = assembly;
        _resourceName = resourceName;
    }

    public ValueTask<Stream> OpenReadAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var assembly = _assembly ?? Assembly.GetEntryAssembly()
            ?? throw new InvalidOperationException("The application assembly is unavailable.");
        var stream = assembly.GetManifestResourceStream(_resourceName)
            ?? throw new InvalidOperationException(
                $"UpdaterStub resource '{_resourceName}' is not embedded in this build.");
        return ValueTask.FromResult(stream);
    }
}

/// <summary>Provides updater stub bytes from memory, useful for custom resource systems.</summary>
public sealed class MemoryUpdaterStubSource(ReadOnlyMemory<byte> bytes) : IUpdaterStubSource
{
    private readonly byte[] _bytes = bytes.ToArray();

    public ValueTask<Stream> OpenReadAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult<Stream>(new MemoryStream(_bytes, writable: false));
    }
}
