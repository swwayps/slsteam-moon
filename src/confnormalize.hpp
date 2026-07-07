#pragma once

// Last-resort text repair for a malformed SLSsteam config.yaml.
//
// The LuaTools frontend edits the AdditionalApps block-list by hand. A writer
// bug could leave the block with INCONSISTENT indentation (some items indented,
// some flush-left), which yaml-cpp rejects for the WHOLE document
// ("end of map not found") -> in the release build the throw can escape the
// catch and abort Steam at startup (boot loop); at best the entire config is
// discarded and every managed game reads as unmanaged.
//
// repairSeqIndent() normalises the indentation of block-sequence items so the
// document parses again. It is intentionally a pure, dependency-free text
// transform (no yaml-cpp, no globals) so it is trivially testable, and it is
// meant to run ONLY after yaml-cpp has already rejected the file. The caller
// re-parses the result and keeps it only if it now parses, so this can never
// make a well-formed file worse. On a file that is already consistent it
// returns the input BYTE-IDENTICAL (changed=false), so it never triggers a
// spurious rewrite / FileWatcher thrash.

#include <string>
#include <vector>

namespace ConfNormalize
{
	namespace detail
	{
		inline bool isBlankOrComment(const std::string& line)
		{
			for (char c : line)
			{
				if (c == ' ' || c == '\t') continue;
				return c == '#';
			}
			return true; // all-whitespace / empty
		}

		// A block-sequence item line: optional leading spaces, then '-' followed
		// by whitespace or end-of-line. Returns the leading-space count via
		// `indent`. Tabs are not valid YAML indentation, so a tab before '-'
		// disqualifies the line (leave such lines untouched).
		inline bool isSeqItem(const std::string& line, size_t& indent)
		{
			size_t i = 0;
			while (i < line.size() && line[i] == ' ') i++;
			if (i >= line.size() || line[i] != '-') return false;
			// "-" must be followed by whitespace or be the whole (trimmed) line.
			const size_t after = i + 1;
			if (after < line.size() && line[after] != ' ' && line[after] != '\t')
				return false;
			indent = i;
			return true;
		}

		inline std::string stripLeadingSpaces(const std::string& line)
		{
			size_t i = 0;
			while (i < line.size() && line[i] == ' ') i++;
			return line.substr(i);
		}

		inline std::vector<std::string> splitLines(const std::string& text,
		                                            bool& trailingNewline)
		{
			std::vector<std::string> lines;
			std::string cur;
			for (char c : text)
			{
				if (c == '\n')
				{
					lines.push_back(cur);
					cur.clear();
				}
				else
				{
					cur.push_back(c);
				}
			}
			trailingNewline = cur.empty() && !text.empty();
			if (!cur.empty()) lines.push_back(cur);
			return lines;
		}
	}

	// See file header. `changed` (optional) is set to whether anything was
	// rewritten. The return value is byte-identical to the input when nothing
	// needed fixing.
	inline std::string repairSeqIndent(const std::string& text,
	                                   bool* changed = nullptr)
	{
		bool trailingNewline = false;
		std::vector<std::string> lines = detail::splitLines(text, trailingNewline);

		bool didChange = false;
		const size_t n = lines.size();
		size_t i = 0;
		while (i < n)
		{
			size_t firstIndent = 0;
			if (!detail::isSeqItem(lines[i], firstIndent))
			{
				i++;
				continue;
			}

			// Extend the run over consecutive seq-item lines, tolerating blank
			// and comment lines interleaved between items (valid inside a YAML
			// block sequence). The run ends at the last actual seq item.
			size_t lastItem = i;
			size_t k = i + 1;
			while (k < n)
			{
				size_t ind = 0;
				if (detail::isSeqItem(lines[k], ind))
				{
					lastItem = k;
					k++;
				}
				else if (detail::isBlankOrComment(lines[k]))
				{
					k++;
				}
				else
				{
					break;
				}
			}

			// Force every seq item in [i, lastItem] to the first item's indent.
			const std::string pad(firstIndent, ' ');
			for (size_t m = i; m <= lastItem; m++)
			{
				size_t ind = 0;
				if (!detail::isSeqItem(lines[m], ind)) continue;
				if (ind == firstIndent) continue;
				lines[m] = pad + detail::stripLeadingSpaces(lines[m]);
				didChange = true;
			}

			i = lastItem + 1;
		}

		if (changed) *changed = didChange;
		if (!didChange) return text; // guarantee byte-identical on no-op

		std::string out;
		for (size_t j = 0; j < lines.size(); j++)
		{
			out += lines[j];
			if (j + 1 < lines.size() || trailingNewline) out += '\n';
		}
		return out;
	}
}
