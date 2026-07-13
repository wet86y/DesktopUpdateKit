# Code provenance

DesktopUpdateKit was created in July 2026 from project-specific requirements
for a reusable Windows desktop application updater. The implementation is
written in C# and the updater stub is compiled to a native Windows executable
with .NET NativeAOT.

Velopack, NetSparkleUpdater, WinSparkle, Squirrel.Windows and libappupdater
were evaluated as architectural alternatives. Their source code is not
included in this repository and DesktopUpdateKit does not link to those
projects.

The repository history starts with the initial standalone implementation and
records the later download, update-session, release-tooling and rollback work.
