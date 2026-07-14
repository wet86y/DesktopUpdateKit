# DesktopUpdateKit native candidate

This directory is a C++20/Win32 candidate implementation of the runtime
download client and updater stub. It is intentionally not referenced by the
current C# host projects or PowerShell release tooling.

Build and test it independently from this repository root:

```powershell
cmake -S .\native -B .\build\native -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build .\build\native --config Release --parallel
ctest --test-dir .\build\native -C Release --output-on-failure
```

The candidate preserves the update manifest, transaction JSON and command-line
contracts used by the managed implementation. It must not be used for a formal
Release or embedded into a host application until the later joint integration
phase with Super Middle Key is complete.
