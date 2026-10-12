// Compile inside the build container so we link against the matching i386 libs:
//   podman run --rm -v $PWD:/build:Z -w /build localhost/slssteam-builder bash -c \
//     'g++ -std=c++20 -m32 -Iinclude tools/test_inject.cpp obj/feats/appinfo_vdf.o obj/log.o obj/config.o obj/utils.o obj/globals.o obj/filewatcher.o obj/update.o obj/api.o obj/feats/depotkey.o lib/libyaml-cpp.a -lcrypto -lpthread -ldl -o /tmp/test_inject'
// Then run on this box (the i386 libcrypto must be installed: libcrypto.so.3:i386):
//   /tmp/test_inject /tmp/appinfo.vdf /tmp/picsbuffer_638510.bin

#include "../src/feats/appinfo_vdf.hpp"
#include "../src/log.hpp"
#include "../src/config.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "usage: " << argv[0] << " <appinfo.vdf> <picsbuffer.bin>\n"; return 1; }

    // Initialize log; appinfo_vdf.cpp depends on g_pLog and on
    // g_config.getDir() (which only reads env, no init needed).
    g_pLog = std::unique_ptr<CLog>(new CLog("/tmp/test_inject.log"));
    setenv("HOME", "/tmp", 1);
    std::system("mkdir -p /tmp/.config/SLSsteam/cache");

    std::ifstream wf(argv[2], std::ios::binary | std::ios::ate);
    if (!wf) { std::cerr << "no wire buffer\n"; return 1; }
    auto sz = wf.tellg();
    std::string wire; wire.resize(sz); wf.seekg(0); wf.read(wire.data(), sz);

    // dotAGE _sha hex (matches what AppInfoProvision decodes)
    const char* hexSha = "7da9fe5518638f1c121bee087f0de0dc0c5dc988";
    std::string sha; sha.assign(20, '\0');
    for (int i = 0; i < 20; ++i) {
        auto v = [](char c) { return (c >= '0' && c <= '9') ? c - '0' : 10 + (c - 'a'); };
        sha[i] = (v(hexSha[2*i]) << 4) | v(hexSha[2*i+1]);
    }

    bool ok = AppInfoVdf::injectApp(argv[1], 638510, 36310070u, sha, wire);
    std::cout << "inject ok=" << ok << "\n";
    return ok ? 0 : 2;
}
