# DesktopUpdateKit native runtime

This directory is the C++20/Win32 implementation of the runtime download client
and updater stub used by the Super Middle Key native migration build. It remains
independent from the current C# host projects and formal PowerShell release tooling.

Build and test it independently from this repository root:

```powershell
cmake -S .\native -B .\build\native -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build .\build\native --config Release --parallel
ctest --test-dir .\build\native -C Release --output-on-failure
```

The native runtime preserves the managed update manifest, transaction JSON and
command-line contracts. It provides streaming WinHTTP transfers, strict Range
validation with parallel-to-single fallback, node failover, pause/cancel control,
SHA-256 verification, health-confirmed replacement and rollback. Formal product
release remains gated by the host application's migration acceptance process.
