# slsteam-moon

A branch of [SLSsteam](https://github.com/AceSLS/SLSsteam) with extra
protocol handlers, SteamStub support, and a Lua manifest importer.

See the [wiki][wiki] for build, install, and configuration.
For just the build flow, see [`docs/BUILDING.md`](docs/BUILDING.md).
For the optional thread-affinity trace and the owner-thread handoff switches,
see [`docs/DIAGNOSTICS.md`](docs/DIAGNOSTICS.md).

[wiki]: https://github.com/swwayps/slsteam-moon/wiki

## Library dates

"Recently added" uses a persistent per-game inclusion date for locally added
games, instead of the shared package's license date. Existing script dates seed
the initial migration; entries without a script use their first observation.
Later script updates and Steam restarts do not change an existing date. Removing
and re-adding a game while Steam observes the changes records a new inclusion.

Dates are stored in `library-added-times.txt` in the SLSsteam configuration
directory, outside the disposable cache. Real license purchase dates remain
unchanged, and explicit `SubscriptionTimestamps` overrides (including `0`)
still take precedence. If the store is unavailable, dates remain stable for
the current session and persistence is retried on the next source reload.

## Credits

Upstream:

- [AceSLS](https://github.com/AceSLS) and contributors —
  the hook framework, protobuf plumbing, ownership patches,
  family-share bypass, and the broader feature set this branch
  builds on.

Reference material:

- [SteamDatabase / SteamAppInfo](https://github.com/SteamDatabase/SteamAppInfo)
  — `appinfo.vdf` v41 format documentation.
- [atom0s / Steamless](https://github.com/atom0s/Steamless) —
  invoked by the wrapper-integration helper.
- [Midrags / SFF / LumaCore](https://github.com/Midrags/SFF) —
  Windows reference port for the `LoadPackage` patch logic.
- [OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) —
  some features were ported from or inspired by this project.
  Full credit to its author.

## Support

Open an issue: https://github.com/swwayps/slsteam-moon/issues
