// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure, IO-free helpers for the SteamStub `--steamless` opt-in. Kept in a
// header so tools/test_steamstub_launchopt.cpp can exercise the parsing and
// token matching without linking the whole injector.
//
// The strip path (feats/steamstub.cpp) is opt-in: a game only goes through
// Steamless when its Steam launch options carry a `-steamless` /
// `--steamless` token. These helpers extract the app's LaunchOptions string
// from a localconfig.vdf blob — scoped to the "apps" section so an appid that
// also appears elsewhere in the file (e.g. a binary rich-presence blob) is not
// matched — and decide whether the opt-in token is present.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace SteamlessLaunchOpt
{
	// Extract the LaunchOptions value for `appId` from a localconfig.vdf blob.
	// Returns "" when the app or the key is absent. Only the "<appId>" block
	// that is a direct child of the "apps" section is considered; VDF
	// backslash escapes in the value are unescaped.
	inline std::string parseAppLaunchOptions(const std::string& vdf, std::uint32_t appId)
	{
		static const std::string appsKey = "\"apps\"";
		std::size_t apps = vdf.find(appsKey);
		if (apps == std::string::npos) return {};

		std::size_t appsOpen = vdf.find('{', apps + appsKey.size());
		if (appsOpen == std::string::npos) return {};

		const std::string needle = "\"" + std::to_string(appId) + "\"";

		// Walk the "apps" scope looking for the appid key at depth 1.
		std::size_t pos = appsOpen + 1;
		int depth = 1;
		std::size_t appOpen = std::string::npos;
		while (pos < vdf.size() && depth >= 1)
		{
			const char c = vdf[pos];
			if (c == '{') { ++depth; ++pos; continue; }
			if (c == '}') { if (--depth == 0) break; ++pos; continue; }
			if (c == '"' && depth == 1
			    && vdf.compare(pos, needle.size(), needle) == 0)
			{
				appOpen = vdf.find('{', pos + needle.size());
				break;
			}
			++pos;
		}
		if (appOpen == std::string::npos) return {};

		// Walk the appid block looking for "LaunchOptions".
		static const std::string loKey = "\"LaunchOptions\"";
		pos = appOpen + 1;
		depth = 1;
		while (pos < vdf.size() && depth >= 1)
		{
			const char c = vdf[pos];
			if (c == '{') { ++depth; ++pos; continue; }
			if (c == '}') { if (--depth == 0) break; ++pos; continue; }
			if (c == '"' && vdf.compare(pos, loKey.size(), loKey) == 0)
			{
				std::size_t vOpen = vdf.find('"', pos + loKey.size());
				if (vOpen == std::string::npos) return {};
				std::string val;
				for (std::size_t i = vOpen + 1; i < vdf.size(); ++i)
				{
					const char ch = vdf[i];
					if (ch == '\\' && i + 1 < vdf.size()) { val += vdf[++i]; continue; }
					if (ch == '"') break;
					val += ch;
				}
				return val;
			}
			++pos;
		}
		return {};
	}

	// True when `opts` carries a `-steamless` / `--steamless` token bounded by
	// whitespace or the string ends (a leading extra '-' is tolerated so both
	// spellings match). Rejects substrings such as "/opt/no-steamlessness".
	inline bool requestsSteamless(const std::string& opts)
	{
		static const std::string tok = "-steamless";
		std::size_t p = 0;
		while ((p = opts.find(tok, p)) != std::string::npos)
		{
			const bool leftOk = (p == 0)
				|| opts[p - 1] == ' ' || opts[p - 1] == '\t' || opts[p - 1] == '-';
			const std::size_t end = p + tok.size();
			const bool rightOk = (end == opts.size())
				|| opts[end] == ' ' || opts[end] == '\t';
			if (leftOk && rightOk) return true;
			p = end;
		}
		return false;
	}
}
