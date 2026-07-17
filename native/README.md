# DesktopUpdateKit native runtime

This directory is the supported C++20/Win32 frontend of DesktopUpdateKit. It is
used by native single-EXE hosts and implements the same v1 behavior contract as
the managed frontend under `src/DesktopUpdateKit`. The two frontends share wire
contracts and fixtures, but neither frontend wraps or loads the other.

Build and test it independently from this repository root:

```powershell
cmake -S .\native -B .\build\native -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build .\build\native --config Release --parallel
ctest --test-dir .\build\native -C Release --output-on-failure
```

The native runtime provides streaming WinHTTP transfers, strict Range validation
with parallel-to-single fallback, node failover, pause/cancel control, SHA-256
verification, health-confirmed replacement and rollback. It is a source-linked
static SDK; no stable binary ABI is promised. Product UI, embedded resource IDs,
asset names and release builds remain host responsibilities.
