// Regression test for manifest extraction inside Steam. Steam owns SIGCHLD and
// may reap helper processes before a plugin can waitpid() them, so extraction
// must remain entirely in-process.

#include "../src/utils/manifest_zip.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>


namespace
{
	int failures = 0;

	#define CHECK(condition, message)                                      \
		do {                                                                 \
			if (condition) std::printf("ok:   %s\n", message);               \
			else { std::printf("FAIL: %s\n", message); ++failures; }         \
		} while (0)

	// A deterministic single-entry ZIP generated independently with Python's
	// raw-DEFLATE zlib API. Keeping the fixture literal means this regression
	// test needs no zlib development headers or link-time dependency.
	constexpr std::array<unsigned char, 119> archiveBytes
	{{
		0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x26, 0x62, 0x53, 0xdd, 0x13, 0x00, 0x00, 0x00, 0x11, 0x00,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x7a, 0xbb, 0x20, 0xfe, 0xad, 0xb0,
		0x24, 0xb5, 0xb8, 0x44, 0x37, 0x37, 0x31, 0x2f, 0x33, 0x2d, 0xb5, 0xb8,
		0x04, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00,
		0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0x62, 0x53, 0xdd, 0x13, 0x00,
		0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x7a, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
		0x00, 0x2f, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
	}};
}


int main()
{
	const std::vector<unsigned char> payload
	{
		0xd0, 0x17, 0xf6, 0x71, // Steam manifest magic 0x71F617D0
		't', 'e', 's', 't', '-', 'm', 'a', 'n', 'i', 'f', 'e', 's', 't',
	};
	const std::string zip(reinterpret_cast<const char*>(archiveBytes.data()),
	                      archiveBytes.size());
	CHECK(!zip.empty(), "test fixture is a non-empty deflated ZIP");

	// Reproduce the host condition that broke fork()+waitpid(): Steam installs
	// its own SIGCHLD handler. An in-process extractor is independent of it.
	std::signal(SIGCHLD, SIG_IGN);
	std::vector<unsigned char> extracted;
	std::string diagnostic;
	CHECK(ManifestZip::extractSingleFile(zip, extracted, &diagnostic),
	      "valid manifest ZIP extracts while SIGCHLD is owned by the host");
	CHECK(extracted == payload, "extracted bytes match the manifest payload");

	std::string corrupt = zip;
	corrupt[14] ^= 0x40; // local-header CRC32
	corrupt[66] ^= 0x40; // matching central-directory CRC32
	extracted.clear();
	CHECK(!ManifestZip::extractSingleFile(corrupt, extracted, &diagnostic),
	      "CRC mismatch is rejected");

	if (failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", failures);
	return 1;
}
