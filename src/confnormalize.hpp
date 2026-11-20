#pragma once

// Last-resort text repair for a malformed SLSsteam config.yaml.
//
// The LuaTools frontend edits the AdditionalApps block-list by hand. A writer
// bug could leave the block with INCONSISTENT indentation (some items indented,
// some flush-left), which yaml-cpp rejects for the WHOLE document
// ("end of map not found") or folds into the wrong list, so the config is
// discarded and every managed game reads as unmanaged.
//
// repairSeqIndent() normalises the indentation of flat block-sequence items so
// the document parses again. It is a pure text transform (no yaml-cpp, no
// globals). ConfLoad::parseWithRepair runs it BEFORE parsing and persists a
// changed result that parses. A run is the items of one list: it ends at an
// item indented less than the key that opened it, a block scalar's text is
// not items, and a run whose items open a nested block is left alone. On a
// file that is already consistent it returns the
// input BYTE-IDENTICAL (changed=false), so it never triggers a spurious
// rewrite / FileWatcher thrash.

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

		// An item whose content continues on deeper lines (a bare "-", an
		// inline nested "- -", a key ending in ':' or a block scalar). Its
		// children are indented on purpose, so its run is not a writer slip.
		inline bool opensBlock(const std::string& line, size_t indent)
		{
			std::string rest = line.substr(indent + 1);
			const size_t comment = rest.find(" #");
			if (comment != std::string::npos) rest.resize(comment);
			const size_t begin = rest.find_first_not_of(" \t");
			if (begin == std::string::npos) return true;
			const size_t end = rest.find_last_not_of(" \t");
			rest = rest.substr(begin, end - begin + 1);
			if (rest[0] == '-' && (rest.size() == 1 || rest[1] == ' ' || rest[1] == '\t'))
				return true;
			const size_t lastToken = rest.find_last_of(" \t");
			const std::string tail =
				lastToken == std::string::npos ? rest : rest.substr(lastToken + 1);
			return rest.back() == ':'
				|| ((tail[0] == '|' || tail[0] == '>')
				    && tail.find_first_not_of("|>+-0123456789") == std::string::npos);
		}

		inline size_t indentOf(const std::string& line)
		{
			size_t i = 0;
			while (i < line.size() && line[i] == ' ') i++;
			return i;
		}
		// A line whose value is a block scalar header ("key: |", "key: >-",
		// "- |"): the lines after it that are blank or deeper are its text.
		inline bool opensBlockScalar(const std::string& line)
		{
			std::string text = line;
			const size_t comment = text.find(" #");
			if (comment != std::string::npos) text.resize(comment);
			const size_t end = text.find_last_not_of(" \t");
			if (end == std::string::npos) return false;
			text.resize(end + 1);
			const size_t lastToken = text.find_last_of(" \t");
			if (lastToken == std::string::npos) return false;
			const std::string tail = text.substr(lastToken + 1);
			if ((tail[0] != '|' && tail[0] != '>')
			    || tail.find_first_not_of("|>+-0123456789") != std::string::npos)
				return false;
			const size_t before = text.find_last_not_of(" \t", lastToken);
			return before != std::string::npos && (text[before] == ':' || text[before] == '-');
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
		// Lines that are a block scalar's text, never items or keys.
		std::vector<bool> scalarText(n, false);
		for (size_t k = 0; k < n; k++)
		{
			if (scalarText[k] || !detail::opensBlockScalar(lines[k])) continue;
			const size_t header = detail::indentOf(lines[k]);
			for (size_t m = k + 1; m < n; m++)
			{
				const bool blank = lines[m].find_first_not_of(" \t") == std::string::npos;
				if (!blank && detail::indentOf(lines[m]) <= header) break;
				scalarText[m] = true;
			}
		}
		size_t i = 0;
		while (i < n)
		{
			size_t firstIndent = 0;
			if (scalarText[i] || !detail::isSeqItem(lines[i], firstIndent))
			{
				i++;
				continue;
			}
			// The key that opened this list. An item indented less than it
			// belongs to an outer list, so it ends the run.
			bool hasOpener = false;
			size_t openerIndent = 0;
			for (size_t p = i; p-- > 0;)
			{
				size_t ind = 0;
				if (scalarText[p] || detail::isBlankOrComment(lines[p])
				    || detail::isSeqItem(lines[p], ind))
					continue;
				hasOpener = true;
				openerIndent = detail::indentOf(lines[p]);
				break;
			}

			// Extend the run over consecutive seq-item lines, tolerating blank
			// and comment lines interleaved between items (valid inside a YAML
			// block sequence). The run ends at the last actual seq item.
			size_t lastItem = i;
			size_t k = i + 1;
			while (k < n)
			{
				size_t ind = 0;
				if (scalarText[k]) break;
				if (detail::isSeqItem(lines[k], ind))
				{
					if (hasOpener && ind < openerIndent) break;
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

			bool nested = false;
			for (size_t m = i; m <= lastItem && !nested; m++)
			{
				size_t ind = 0;
				nested = detail::isSeqItem(lines[m], ind) && detail::opensBlock(lines[m], ind);
			}
			if (nested)
			{
				i = lastItem + 1;
				continue;
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
