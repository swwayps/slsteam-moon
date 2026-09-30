# Builder image for slsteam-moon's portable build.
#
# Ubuntu 22.04 supplies the release toolchain. Its glibc is 2.35, while the
# produced helper is policy-gated to require no symbols newer than GLIBC_2.34.
# Resulting binaries target Ubuntu 22.04+, Debian 12+, Fedora 36+, and modern
# Mint/Pop!_OS/Zorin releases.
#
# Used by scripts/build.sh --portable (which invokes
# scripts/_build-portable.sh). Source is bind-mounted at run time;
# this image only carries the toolchain.
FROM ubuntu:22.04

# Install 32-bit toolchain and the dev headers the Makefile links.
# (-m32 builds; SLSsteam.so must match Steam's 32-bit ubuntu12_32 client.)
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get \
        -o Dpkg::Options::="--force-overwrite" \
        install -y --no-install-recommends \
        ca-certificates \
        g++-multilib \
        libcurl4-openssl-dev:amd64 \
        libcurl4-openssl-dev:i386 \
        libssl-dev:amd64 \
        libssl-dev:i386 \
        make \
        pkg-config && \
    rm -rf /var/lib/apt/lists/*

# LuaJIT headers for the Lua plugin runtime (<luajit-2.1/lua.hpp>); the static
# library itself is vendored in lib/. A separate layer keeps the toolchain
# layer above cached.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libluajit-5.1-dev:i386 && \
    rm -rf /var/lib/apt/lists/*

# 32-bit pkg-config search path (matches scripts/_build-host.sh).
ENV PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig

WORKDIR /build
