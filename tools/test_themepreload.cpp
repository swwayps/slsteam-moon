// Build: g++ -std=c++20 tools/test_themepreload.cpp -o /tmp/test_themepreload
//        && /tmp/test_themepreload
#include "../src/feats/themepreload.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("FAIL: %s\n", m); ++failures; } } while (0)

static std::string read(const std::filesystem::path& path)
{
	std::ifstream f(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static void write(const std::filesystem::path& path, const std::string& body)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary | std::ios::trunc) << body;
}

int main()
{
	const std::string vanilla =
		"<!doctype html><head><script defer=\"defer\" src=\"/libraries/vendor.js\"></script>"
		"<script defer=\"defer\" src=\"/library.js\"></script></head><body></body>";
	auto [patched, err] = ThemePreload::patchHtml(vanilla);
	CHECK(err.empty(), "known Steam index is accepted");
	CHECK(patched.find(ThemePreload::kGuardStart) < patched.find("/libraries/vendor.js"),
	      "guard is parser-blocking before Valve's first script");
	CHECK(patched.find("/library.js") < patched.find(ThemePreload::kRuntimeStart),
	      "compiled runtime is deferred after library.js");

	auto [again, againErr] = ThemePreload::patchHtml(patched);
	CHECK(againErr.empty() && again == patched, "HTML patching is idempotent");
	auto [clean, cleanErr] = ThemePreload::cleanHtml(patched);
	CHECK(cleanErr.empty() && clean == vanilla, "cleanup restores the exact vanilla index");
	auto [bad, badErr] = ThemePreload::patchHtml("<html><script src=\"/other.js\"></script></html>");
	CHECK(!badErr.empty() && bad.empty(), "unknown layouts fail closed");
	const auto root = std::filesystem::path("/tmp") /
		("lumen-native-theme-preload-" + std::to_string(getpid()));
	const auto stage = root / "stage";
	const auto ui = root / "steamui";
	std::filesystem::remove_all(root);
	write(stage / ThemePreload::kGuardFile, "guard-source");
	write(stage / ThemePreload::kRuntimeFile, "runtime-source");
	const auto theme = root / "theme";
	std::filesystem::create_directories(theme);
	write(stage / ThemePreload::kAssetPathFile, theme.string());
	write(ui / "index.html", vanilla);
	std::string publishErr;
	CHECK(ThemePreload::publish(stage, ui, publishErr), "staged theme publishes at webhelper exec");
	CHECK(publishErr.empty(), "successful publish has no error");
	CHECK(read(ui / ThemePreload::kGuardFile) == "guard-source", "guard copied after Steam verification");
	CHECK(read(ui / ThemePreload::kRuntimeFile) == "runtime-source", "runtime copied after Steam verification");
	CHECK(std::filesystem::is_symlink(ui / ThemePreload::kAssetLink),
	      "theme assets are exposed without copying binaries into the bootstrap");
	CHECK(std::filesystem::read_symlink(ui / ThemePreload::kAssetLink) == theme,
	      "native asset mount points at the staged active theme root");
	CHECK(read(ui / "index.html").find(ThemePreload::kGuardStart) != std::string::npos,
	      "live index is patched only at publish time");

	std::filesystem::remove_all(root);
	if (failures) return 1;
	std::printf("ok - native SteamUI theme publish gate\n");
	return 0;
}
