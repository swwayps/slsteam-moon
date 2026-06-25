# slsteam-moon

A branch of [SLSsteam](https://github.com/AceSLS/SLSsteam) with extra
protocol handlers, SteamStub support, and a Lua manifest importer.

See the [wiki][wiki] for build, install, and configuration.
For just the build flow, see [`docs/BUILDING.md`](docs/BUILDING.md).

[wiki]: https://github.com/luatools-linux/slsteam-moon/wiki

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

## Support

Open an issue: https://github.com/luatools-linux/slsteam-moon/issues
