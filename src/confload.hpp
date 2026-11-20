#pragma once

// Parse-with-repair decision layer for config.yaml.
//
// Sits between the raw file text and CConfig::loadSettings. It guarantees that
// a malformed config can never take Steam down at startup:
//
//   * a clean file parses as-is (Outcome::ParsedAsIs) and is left untouched;
//   * a file whose block-sequence items have inconsistent indentation (the
//     known LuaTools AdditionalApps writer slip) is normalised via
//     ConfNormalize::repairSeqIndent and parsed from the fixed text
//     (Outcome::Repaired). `repaired` carries the text the caller should
//     persist so the user's game list survives and the file is clean for the
//     FileWatcher / the plugin;
//   * anything that still cannot be parsed reports Outcome::Failed with an
//     empty node, so the caller falls back to built-in defaults and boots.
//
// It never throws (every YAML::Load is wrapped). The normalised text is parsed
// first; repairSeqIndent changes only items of one list that are indented
// differently, so a valid nested structure is not rewritten.

#include "confnormalize.hpp"
#include "yaml-cpp/yaml.h"

#include <string>

namespace ConfLoad
{
	enum class Outcome
	{
		ParsedAsIs, // raw text parsed cleanly; nothing to persist
		Repaired,   // indentation was normalised; persist `repaired`
		Failed,     // unparseable even after repair; use defaults
	};

	namespace detail
	{
		// A config is a map or empty; reading a key from any other root
		// (plain text, a list) throws, so such a document counts as unparsed.
		inline bool tryLoad(const std::string& text, YAML::Node& out)
		{
			try
			{
				out = YAML::Load(text);
				return out.IsMap() || out.IsNull();
			}
			catch (...)
			{
				return false;
			}
		}
	}

	// See header. `node` receives the parsed document (or an empty node on
	// Failed); `repaired` receives the normalised text (meaningful only when
	// the return value is Repaired).
	inline Outcome parseWithRepair(const std::string& raw, YAML::Node& node,
	                               std::string& repaired)
	{
		bool changed = false;
		repaired = ConfNormalize::repairSeqIndent(raw, &changed);

		if (changed)
		{
			// The raw file has inconsistent block-sequence indentation, which
			// yaml-cpp either rejects outright or silently mis-folds. Parse the
			// normalised text FIRST: it avoids the throwing path entirely and
			// yields the correct list even in the silent-fold case.
			if (detail::tryLoad(repaired, node)) return Outcome::Repaired;
			// Repair didn't make it parseable (some other malformation). Fall
			// back to the raw text in case it was parseable-but-quirky anyway.
			if (detail::tryLoad(raw, node)) return Outcome::ParsedAsIs;
			node = YAML::Node();
			return Outcome::Failed;
		}

		if (detail::tryLoad(raw, node)) return Outcome::ParsedAsIs;
		node = YAML::Node();
		return Outcome::Failed;
	}
}
