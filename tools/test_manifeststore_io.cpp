// Standalone filesystem test for atomic manifest store operations.
//
// Build:
//   g++ -std=c++20 -I include tools/test_manifeststore_io.cpp \
//       -o /tmp/test_manifeststore_io && /tmp/test_manifeststore_io

#include "../src/feats/manifeststore_io.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static void writeManifest(const fs::path& path, uint32_t magic,
                          const std::string& payload)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
	out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

static std::string readAll(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main()
{
	char tmpTemplate[] = "/tmp/slsteam_manifeststore_test_XXXXXX";
	const char* tmp = mkdtemp(tmpTemplate);
	if (!tmp)
	{
		std::perror("mkdtemp");
		return 1;
	}

	const fs::path root = tmp;
	const fs::path store = root / "store";
	const fs::path depotcache = root / "depotcache";
	const fs::path source = root / "downloaded.manifest";
	const std::string name = "220201_6331816835925954858.manifest";
	const fs::path stored = store / name;
	const fs::path staged = depotcache / name;

	// A valid downloaded manifest is persisted before being materialized for
	// Steam. Both copies must contain exactly the same bytes.
	writeManifest(source, 0x71F617D0u, "payload-v1");
	CHECK(ManifestStoreIO::publish(source, stored, staged),
	      "valid manifest publishes to store and depotcache");
	CHECK(ManifestStoreIO::isValidManifest(stored),
	      "persistent copy is a valid manifest");
	CHECK(ManifestStoreIO::isValidManifest(staged),
	      "depotcache copy is a valid manifest");
	CHECK(readAll(source) == readAll(stored) && readAll(stored) == readAll(staged),
	      "published copies preserve the exact bytes");

	// Steam may purge depotcache. Restore must use the persistent copy without
	// contacting the network.
	fs::remove(staged);
	CHECK(ManifestStoreIO::restore(stored, staged),
	      "purged manifest restores from persistent store");
	CHECK(readAll(stored) == readAll(staged),
	      "restored depotcache copy matches the store");

	// Invalid/corrupt files must never enter either fallback location.
	const fs::path badSource = root / "bad.manifest";
	const fs::path badStored = store / "1_2.manifest";
	const fs::path badStaged = depotcache / "1_2.manifest";
	writeManifest(badSource, 0xDEADBEEFu, "not-a-steam-manifest");
	CHECK(!ManifestStoreIO::publish(badSource, badStored, badStaged),
	      "invalid manifest is rejected");
	CHECK(!fs::exists(badStored) && !fs::exists(badStaged),
	      "invalid manifest leaves no store or depotcache copy");

	// The preferred marker records observation order explicitly. Gids remain
	// opaque and are never compared numerically.
	CHECK(ManifestStoreIO::writePreferred(store, 220201, 20),
	      "preferred gid marker is written atomically");
	CHECK(ManifestStoreIO::readPreferred(store, 220201, 0) == 20,
	      "preferred gid marker is read back");
	CHECK(ManifestStoreIO::readPreferred(store, 220201, 20) == 0,
	      "failed exact gid is excluded from preferred fallback");
	CHECK(ManifestStoreIO::writePreferred(store, 220201, 3),
	      "newer observation may have a numerically smaller gid");
	CHECK(ManifestStoreIO::readPreferred(store, 220201, 0) == 3,
	      "latest observation replaces the marker without numeric ordering");

	// Atomic helpers must not leave temporary files after successful writes.
	bool foundTmp = false;
	for (const auto& entry : fs::recursive_directory_iterator(root))
	{
		if (entry.path().filename().string().find(".slsteam_tmp.") !=
		    std::string::npos)
		{
			foundTmp = true;
			break;
		}
	}
	CHECK(!foundTmp, "successful operations leave no temporary files");

	fs::remove_all(root);

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
