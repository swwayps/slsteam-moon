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

# Install the x86-64 toolchain and the dev headers the Makefile links.
# (SLSsteam.so must match the 64-bit client in steamrt64/.)
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get \
        -o Dpkg::Options::="--force-overwrite" \
        install -y --no-install-recommends \
        ca-certificates \
        g++ \
        libcurl4-openssl-dev \
        libssl-dev \
        make \
        pkg-config && \
    rm -rf /var/lib/apt/lists/*

# LuaJIT headers for the Lua plugin runtime (<luajit-2.1/lua.hpp>); the static
# library itself is vendored in lib/. A separate layer keeps the toolchain
# layer above cached.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libluajit-5.1-dev && \
    rm -rf /var/lib/apt/lists/*

# x86-64 pkg-config search path.
ENV PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig

WORKDIR /build
