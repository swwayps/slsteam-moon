// Standalone test for Utils::strsplit.
//
// The old implementation was:
//     char* split = strtok(str, delimeter);
//     splits.emplace(splits.end(), std::string(split));   // split may be NULL
// strtok returns NULL for an empty string or one made only of delimiters, and
// std::string(nullptr) is undefined behaviour — in practice a crash of the whole
// Steam client. api.cpp reaches it directly: fstream.getline() over an empty line
// yields "", so with the API enabled `printf '\n' > <api file>` took the client
// down. api.cpp then also read split[0] before checking split.size().
//
// Two further defects: strtok MUTATES the buffer it is given (in cleanEnvVar that
// buffer belongs to environ, so the process' own environment was rewritten), and
// strtok keeps global state, so two threads splitting at once corrupt each
// other's iteration — this is called from the API watcher thread and from the
// pattern-scan path.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_strsplit.cpp src/utils.cpp \
//       -o /tmp/test_strsplit && /tmp/test_strsplit
#include "../src/utils.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            std::printf("ok   %s\n", (msg));                                   \
        } else {                                                              \
            std::printf("FAIL %s\n", (msg));                                   \
            g_failures++;                                                      \
        }                                                                     \
    } while (0)

int main()
{
	// ── the crash ────────────────────────────────────────────────────────────
	{
		// Empty input: strtok returns NULL here. Must be an empty vector, not UB.
		auto out = Utils::strsplit("", "|");
		CHECK(out.empty(), "empty input yields no fields");
	}
	{
		// Only delimiters: strtok also returns NULL.
		auto out = Utils::strsplit("|", "|");
		CHECK(out.empty(), "a lone delimiter yields no fields");
		out = Utils::strsplit("|||", "|");
		CHECK(out.empty(), "repeated delimiters yield no fields");
	}
	{
		auto out = Utils::strsplit(nullptr, "|");
		CHECK(out.empty(), "null input yields no fields");
	}
	{
		auto out = Utils::strsplit("a|b", nullptr);
		CHECK(out.size() == 1 && out[0] == "a|b",
		      "a null delimiter set leaves the input whole");
		out = Utils::strsplit("a|b", "");
		CHECK(out.size() == 1 && out[0] == "a|b",
		      "an empty delimiter set leaves the input whole");
	}

	// ── ordinary splitting (unchanged behaviour) ─────────────────────────────
	{
		auto out = Utils::strsplit("install|480|0", "|");
		CHECK(out.size() == 3 && out[0] == "install" && out[1] == "480"
		          && out[2] == "0",
		      "three fields split in order");
	}
	{
		// strtok collapses runs of delimiters and ignores leading/trailing ones.
		// Preserve that: callers index by position and were written against it.
		auto out = Utils::strsplit("|a||b|", "|");
		CHECK(out.size() == 2 && out[0] == "a" && out[1] == "b",
		      "empty fields are collapsed, as strtok did");
	}
	{
		// Multiple delimiter characters, as used for $LD_AUDIT (":") and for
		// capstone operand lists (",").
		auto out = Utils::strsplit("rax, qword ptr [rip + 0x10]", ",");
		CHECK(out.size() == 2 && out[0] == "rax"
		          && out[1] == " qword ptr [rip + 0x10]",
		      "only the delimiter splits; spaces are preserved");
	}
	{
		auto out = Utils::strsplit("a:b,c", ":,");
		CHECK(out.size() == 3 && out[0] == "a" && out[1] == "b" && out[2] == "c",
		      "every character of the delimiter set splits");
	}
	{
		auto out = Utils::strsplit("single", "|");
		CHECK(out.size() == 1 && out[0] == "single",
		      "input without a delimiter is one field");
	}

	// ── the input is not modified ───────────────────────────────────────────
	{
		// cleanEnvVar passes getenv()'s pointer, which points into environ.
		// strtok wrote NULs into it, rewriting the process' own environment.
		char buffer[] = "one:two:three";
		char original[sizeof(buffer)];
		std::memcpy(original, buffer, sizeof(buffer));
		auto out = Utils::strsplit(buffer, ":");
		CHECK(out.size() == 3, "a mutable buffer still splits");
		CHECK(std::memcmp(buffer, original, sizeof(buffer)) == 0,
		      "the caller's buffer is left byte-identical");
	}

	// ── usable from more than one thread ────────────────────────────────────
	{
		// strtok's cursor is global (or per-thread at best), so two concurrent
		// splits interfered. Run many in parallel and require every result to be
		// exactly what a serial call produces.
		constexpr int kThreads = 8;
		constexpr int kIterations = 2000;
		std::vector<std::thread> threads;
		std::vector<int> bad(kThreads, 0);
		for (int t = 0; t < kThreads; t++) {
			threads.emplace_back([t, &bad]() {
				const std::string input =
				    "field" + std::to_string(t) + "|480|0";
				for (int i = 0; i < kIterations; i++) {
					auto out = Utils::strsplit(input.c_str(), "|");
					if (out.size() != 3 || out[0] != "field" + std::to_string(t)
					    || out[1] != "480" || out[2] != "0") {
						bad[t]++;
					}
				}
			});
		}
		for (auto& th : threads) th.join();
		int total = 0;
		for (int b : bad) total += b;
		CHECK(total == 0, "concurrent splits do not interfere");
	}

	// ── long input is not truncated ─────────────────────────────────────────
	{
		std::string input;
		for (int i = 0; i < 500; i++) {
			if (i) input += ":";
			input += std::to_string(i);
		}
		auto out = Utils::strsplit(input.c_str(), ":");
		CHECK(out.size() == 500 && out.front() == "0" && out.back() == "499",
		      "500 fields all survive");
	}

	if (g_failures == 0) {
		std::printf("test_strsplit: ALL PASS\n");
		return 0;
	}
	std::printf("test_strsplit: %d FAILURE(S)\n", g_failures);
	return 1;
}
