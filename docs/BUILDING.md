# Building slsteam-moon

slsteam-moon is a 32-bit shared library that hooks into Steam through
`LD_AUDIT`. The build produces two artefacts:

- `bin/SLSsteam.so` — main hook library, the only `LD_AUDIT` module
- `bin/pattern-refresh` — native helper the wrapper runs before Steam to
  refresh the signed pattern catalog

## Quick start

| Goal                                  | Command                          |
| ------------------------------------- | -------------------------------- |
| Use it (don't build)                  | grab the latest [release][rel]   |
| Develop / iterate fast                | `scripts/build.sh --host`        |
| Build a portable, release-grade binary| `scripts/build.sh --portable`    |
| Produce the release zip               | `scripts/release.sh --version X.Y-lumen` |
| Install onto your own Steam           | `make install`                   |

[rel]: https://github.com/swwayps/slsteam-moon/releases/latest

`make` on its own is equivalent to `scripts/build.sh --host`.

## Build modes

### Host build (`scripts/build.sh --host` / `make`)

Compiles in place using the host toolchain. Fast, no container needed.

The catch: the binary's glibc requirement matches the system you build
on. A binary built on Ubuntu 24.04 (glibc 2.39) will not run on Ubuntu
22.04 (glibc 2.34). Use this for development on whatever distro you're
on; use the portable mode for anything you ship.

Dependencies (Ubuntu/Debian):

```
sudo apt install g++ make pkg-config libcurl4-openssl-dev libssl-dev
```

Arch:

```
sudo pacman -S gcc make pkgconf openssl curl
```

The `--host` script checks for g++, make and pkg-config, and prints these
lines when one is missing.

### Portable build (`scripts/build.sh --portable`)

Builds inside an Ubuntu 22.04 container (Podman or Docker), so the
resulting binary works on any distro with **glibc ≥ 2.34**: Ubuntu
22.04+, Debian 12+, Fedora 36+, modern Mint, Pop!_OS, Zorin, etc.

This is what release builds use. Requires Podman (rootless, preferred)
or Docker installed; the script picks whichever it finds.

The container image (`slsteam-moon-builder`) is built from
`scripts/Dockerfile` on first run; subsequent runs reuse the cache.

### Cross-distro reach

The portable build's glibc minimum is determined by the base image, not
by anything in our code. If you need to support older distros (e.g.
glibc 2.31 / Ubuntu 20.04), change the `FROM` line in
`scripts/Dockerfile` to `ubuntu:20.04` and rebuild. The Makefile flags
already include `-D_GLIBCXX_USE_CXX11_ABI=0` for C++ ABI compatibility.

## Releasing
```
scripts/release.sh --version 2.9-lumen
```
Equivalent to `scripts/build.sh --portable` followed by
`scripts/package.sh`. The output is `dist/slsteam-moon-linux-<ver>.zip`
plus its `.sha256` checksum. Both installers (`autofix.sh` and
`luatools-moon/install.sh`) only pick an asset whose name ends in `-lumen.zip`
and verify it against the checksum file, so `<ver>` must end in `-lumen` and
both files are uploaded. Layout:
```
slsteam-moon-<ver>/
  bin/{SLSsteam.so, pattern-refresh}
  setup.sh
  ensure-desktop-coverage.sh
  res/config.yaml
  docs/LICENSE/
  tools/{desktop-coverage,desktop-guardian-units,launcher-shim}.lib.sh
  tools/steamstub-bypass/
  tools/steamless-bin/
```
Upload both files to the GitHub release with `gh`:
```
gh release upload vX.Y dist/slsteam-moon-linux-X.Y-lumen.zip \
    dist/slsteam-moon-linux-X.Y-lumen.zip.sha256 --clobber
```
## Verifying a build

```
scripts/check-abi.sh bin/SLSsteam.so --x86-64
scripts/check-abi.sh bin/pattern-refresh
ldd --version | head -1
```

`check-abi.sh` fails when a binary needs a glibc newer than 2.34, the
release floor (`scripts/package.sh` runs the same check). `ldd --version`
prints the glibc of the current host.

You can also check by hand:

```
objdump -T bin/SLSsteam.so | grep GLIBC | sort -V | tail -5
ldd bin/SLSsteam.so
```

## Troubleshooting

**`wrong ELF class: ELFCLASS32`** — you're trying to inject into a
64-bit Steam process. The Steam Linux client runs both `steam`
(launcher, 64-bit) and `ubuntu12_32/steam` (real client, 32-bit).
slsteam-moon only hooks the 32-bit client; `setup.sh` handles this.

**`GLIBC_X.YZ not found` at Steam launch** — the binary was built on a
newer system than the user's. Rebuild with `scripts/build.sh --portable`,
or pick an older base image in `scripts/Dockerfile`.

**`cannot open shared object file` for libssl/libcurl** — missing runtime
libraries on the host:
```
sudo apt install libssl3 libcurl4      # Ubuntu/Debian
sudo pacman -S openssl curl            # Arch
```

**Docker permission denied** — add yourself to the docker group, or use
podman (rootless):

```
sudo usermod -aG docker $USER && newgrp docker
```

## Build flags

From the Makefile:

```
-O3 -flto=auto -fPIC -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0
```
- 64-bit (no `-m32`) — this build targets the `steamrt64` client.
- `-D_GLIBCXX_USE_CXX11_ABI=0` — old C++ ABI for compatibility with
  the system `libstdc++.so.6` Steam loads.
- `-O3 -flto=auto` — size + speed.
- `-fPIC` — required for shared libraries.

Don't change these without a reason; they're the same flags the
upstream binaries have shipped with for years.
