// TDD regression test for best-effort file hashing.

#include "utils.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}
}

int main()
{
	const std::string missing = "/tmp/slsteam-test-utils-sha-missing";
	std::remove(missing.c_str());
	check(Utils::getFileSHA256(missing.c_str()).empty(),
	      "missing file returns an empty digest");

	const std::string path = "/tmp/slsteam-test-utils-sha-input";
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << "abc";
	}
	check(Utils::getFileSHA256(path.c_str()) ==
	          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	      "readable file keeps SHA-256 behavior");
	// Lengths that reach the two-block padding (56 and 64 bytes), cross the
	// 64 KiB read buffer (65,537) and stream many blocks (1,000,000). The
	// digests come from FIPS 180-2 and from coreutils sha256sum.
	const struct
	{
		std::string body;
		const char* digest;
		const char* what;
	} vectors[] = {
		{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		 "an empty file"},
		{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
		 "a 56-byte file (padding takes a second block)"},
		{std::string(64, 'a'),
		 "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
		 "a 64-byte file"},
		{std::string(65537, 'b'),
		 "00056d4dbd0981b55e459d5b86bd544d5871ca666787e16992d56df358d1ea07",
		 "a file one byte past the read buffer"},
		{std::string(1000000, 'a'),
		 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
		 "a 1,000,000-byte file"},
	};
	for (const auto& vector : vectors)
	{
		{
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << vector.body;
		}
		check(Utils::getFileSHA256(path.c_str()) == vector.digest, vector.what);
	}
	std::remove(path.c_str());

	if (failures != 0)
	{
		std::fprintf(stderr, "test_utils_sha: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("utils SHA tests passed");
	return 0;
}
