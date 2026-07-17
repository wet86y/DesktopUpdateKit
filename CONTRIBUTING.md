# Contributing

Contributions are welcome through issues and pull requests.

By submitting a contribution, you agree that your contribution is licensed
under the MIT License that applies to this repository. Only submit code and
assets that you have the right to license. Identify third-party code and its
license in the pull request and update `THIRD-PARTY-NOTICES.md` when needed.

Keep update operations explicit, preserve SHA-256 and size verification, and
do not weaken rollback or clean-worktree release checks. Run the consuming
application's self-check and update acceptance tests before proposing changes
to the update protocol.

Managed and native are equal supported frontends of the v1 behavior contract.
A protocol or runtime behavior change must update both implementations and the
shared fixtures, or explicitly document why it is host-specific. Run both
`dotnet test` and `native\scripts\build-native.ps1 -Configuration Release`.
