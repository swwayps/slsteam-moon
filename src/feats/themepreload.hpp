#pragma once

// Publish Lumen's already-compiled theme bootstrap at the one safe point in a
// Steam boot: inside the existing exec interposer, after Steam's updater has
// verified/extracted the client and immediately before steamwebhelper starts.
// Nothing here parses theme metadata or runs in the steady state. The wrapper
// opts in with environment paths only for an active custom theme.

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <unistd.h>

namespace ThemePreload
{
	inline constexpr const char* kGuardStart = "<!-- lumen-theme-preload:start -->";
	inline constexpr const char* kGuardEnd = "<!-- lumen-theme-preload:end -->";
	inline constexpr const char* kRuntimeStart = "<!-- lumen-theme-runtime:start -->";
	inline constexpr const char* kRuntimeEnd = "<!-- lumen-theme-runtime:end -->";
	inline constexpr const char* kGuardFile = "lumen-theme-preload.js";
	inline constexpr const char* kRuntimeFile = "lumen-theme-runtime.js";
	inline constexpr const char* kAssetPathFile = "lumen-theme-assets.path";
	inline constexpr const char* kAssetLink = "lumen-theme-assets";

	inline bool readFile(const std::filesystem::path& path, std::string& out)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) return false;
		out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return static_cast<bool>(f) || f.eof();
	}

	inline bool atomicWrite(const std::filesystem::path& path, const std::string& body,
	                        std::string& error)
	{
		const auto temp = path.string() + ".tmp.lumen." + std::to_string(::getpid());
		{
			std::ofstream f(temp, std::ios::binary | std::ios::trunc);
			if (!f || !(f << body))
			{
				error = "could not write " + temp;
				std::error_code ignored;
				std::filesystem::remove(temp, ignored);
				return false;
			}
		}
		std::error_code ec;
		std::filesystem::rename(temp, path, ec);
		if (ec)
		{
			error = "could not replace " + path.string() + ": " + ec.message();
			std::filesystem::remove(temp, ec);
			return false;
		}
		return true;
	}

	inline bool atomicAssetLink(const std::filesystem::path& link,
	                            const std::filesystem::path& target,
	                            std::string& error)
	{
		std::error_code ec;
		if (!target.is_absolute() || !std::filesystem::is_directory(target, ec) || ec)
		{
			error = "staged Lumen theme asset root is not an absolute directory";
			return false;
		}
		const auto status = std::filesystem::symlink_status(link, ec);
		if (!ec && std::filesystem::is_symlink(status))
		{
			const auto current = std::filesystem::read_symlink(link, ec);
			if (!ec && current == target) return true;
		}
		else if (!ec && std::filesystem::exists(status))
		{
			error = "SteamUI theme asset path is not a symlink";
			return false;
		}
		ec.clear();
		const auto temp = std::filesystem::path(link.string() + ".tmp.lumen." +
		                                       std::to_string(::getpid()));
		std::filesystem::remove(temp, ec);
		ec.clear();
		std::filesystem::create_directory_symlink(target, temp, ec);
		if (ec)
		{
			error = "could not create SteamUI theme asset link: " + ec.message();
			return false;
		}
		std::filesystem::rename(temp, link, ec);
		if (ec)
		{
			error = "could not publish SteamUI theme asset link: " + ec.message();
			std::filesystem::remove(temp, ec);
			return false;
		}
		return true;
	}

	inline bool removeBlock(std::string& body, const std::string& first,
	                        const std::string& last, std::string& error)
	{
		while (true)
		{
			const auto begin = body.find(first);
			if (begin == std::string::npos) return true;
			const auto end = body.find(last, begin + first.size());
			if (end == std::string::npos)
			{
				error = "unterminated Lumen theme marker";
				return false;
			}
			body.erase(begin, end + last.size() - begin);
		}
	}

	inline std::pair<std::string, std::string> cleanHtml(const std::string& original)
	{
		std::string body = original;
		std::string error;
		if (!removeBlock(body, kGuardStart, kGuardEnd, error) ||
		    !removeBlock(body, kRuntimeStart, kRuntimeEnd, error))
		{
			return {"", error};
		}
		return {body, ""};
	}

	inline std::pair<std::string, std::string> patchHtml(const std::string& original)
	{
		auto [body, error] = cleanHtml(original);
		if (!error.empty()) return {"", error};

		const auto firstScript = body.find("<script");
		const auto librarySrc = body.find("src=\"/library.js\"");
		if (firstScript == std::string::npos || librarySrc == std::string::npos)
			return {"", "SteamUI index anchors were not recognized"};
		const auto libraryOpen = body.rfind("<script", librarySrc);
		const auto libraryCloseStart = body.find("</script>", librarySrc);
		if (libraryOpen == std::string::npos || libraryCloseStart == std::string::npos)
			return {"", "SteamUI library.js tag is incomplete"};
		const auto libraryClose = libraryCloseStart + std::string("</script>").size();

		const std::string guard = std::string(kGuardStart) +
			"<script src=\"/" + kGuardFile + "\"></script>" + kGuardEnd;
		const std::string runtime = std::string(kRuntimeStart) +
			"<script defer=\"defer\" src=\"/" + kRuntimeFile + "\"></script>" +
			kRuntimeEnd;
		body.insert(libraryClose, runtime);
		body.insert(firstScript, guard);
		return {body, ""};
	}

	inline bool publish(const std::filesystem::path& staging,
	                    const std::filesystem::path& steamui, std::string& error)
	{
		std::string guard;
		std::string runtime;
		std::string assetPath;
		std::string index;
		if (!readFile(staging / kGuardFile, guard) ||
		    !readFile(staging / kRuntimeFile, runtime) ||
		    !readFile(staging / kAssetPathFile, assetPath))
		{
			error = "staged Lumen theme helpers are missing";
			return false;
		}
		if (!readFile(steamui / "index.html", index))
		{
			error = "SteamUI index.html is missing";
			return false;
		}
		auto [patched, patchError] = patchHtml(index);
		if (!patchError.empty())
		{
			error = patchError;
			return false;
		}

		// Publish dependencies first; index.html is the atomic commit point.
		if (!atomicWrite(steamui / kGuardFile, guard, error) ||
		    !atomicWrite(steamui / kRuntimeFile, runtime, error) ||
		    !atomicAssetLink(steamui / kAssetLink,
		                     std::filesystem::path(assetPath), error) ||
		    !atomicWrite(steamui / "index.html", patched, error))
			return false;
		return true;
	}
}
