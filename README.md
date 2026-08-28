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

## Achievement scope

Adding an owned base game for DLC does not opt its achievements into local
tracking. The original Steam license result is checked before any overrides:
real packages (including shared/expired licenses), native package-0 entries,
unknown licenses and other users' queries keep the official path. Only entries
actually appended to package 0 are eligible for local schema handling.

Legacy and modern stats requests are correlated by their final CM job IDs.
Account/license changes reject stale replies; failed or oversized schema replies
never become successful zero-progress responses. No existing local achievements
are replayed to a Steam profile.

Update **both SLSsteam and CloudRedirect** for this fix. CloudRedirect uses the
versioned `slsteam_local_stats_epoch_v1` C bridge across the audit/preload boundary.
An unavailable bridge fails closed. If the account changes without restarting
the Steam process, CloudRedirect stays on the official path until a full Steam
restart reinitializes its account-scoped store. Cloud save routing is unchanged.

Regression checks: `make test-achievements test-achievement-scope
test-stats-provenance test-stats-audit test-audit-symbols test-audit-policy`
(run the ELF32 targets in the portable builder).

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
