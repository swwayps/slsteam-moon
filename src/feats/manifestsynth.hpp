// SPDX-License-Identifier: AGPL-3.0-only
//
// manifestsynth — rebuild a missing appinfo `depots` block from manifests
// we already hold on disk.
//
// Why this exists
// ---------------
// A handful of titles (e.g. Risk of Rain 2, app 632360) gate their PICS
// product-info behind an app access token that Valve DENIES to anonymous
// sessions (the appid is returned in app_denied_tokens, never with a
// usable token).  Provisioning uses an anonymous CM session by design (it
// must never touch the user's live session — that trips the changelist
// SHA-1 "Loading user data" loop), so the product-info it receives for
// such a title carries NO `depots` block.  provisionApp then bails
// ("buffer has no depots") and Steam shows the app as 0 B.
//
// The data isn't actually missing, though: the LuaTools per-game zip
// shipped the depot manifest (archived in the ManifestStore) and the depot
// decryption key (in our DepotKey cache).  So we hold the depot id, its
// manifest gid, and its key — everything Steam needs to plan + decrypt the
// install.  ManifestSynth rebuilds the absent `depots` block from those
// (depotId -> gid) pairs, in the exact wire shape Steam's appinfo uses, so
// the normal prune/splice tail (pruneUnsupportedDepots, the Proton
// CompatTool mapping for windows-only depots, the synchronous manifest
// staging) runs unchanged and the first-attempt install succeeds.
//
// Kept free of globals/I/O (only yaml-cpp + std) so it is host-unit-
// testable (tools/test_manifestsynth.cpp); the disk gather (which depots a
// title owns, and their best archived gid) lives in
// appinfo_provision.cpp::gatherSynthDepots.

#pragma once

#include "../ascii.hpp"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ManifestSynth
{
	// One depot we can synthesize an appinfo entry for.
	struct SynthDepot
	{
		uint32_t    depotId  = 0;
		uint64_t    gid      = 0;   // archived public-branch manifest gid (0 = none)
		std::string oslist;         // "" -> omit config.oslist (unknown platform)
		uint64_t    size     = 0;   // cb_disk_original   (0 = unknown -> omit)
		uint64_t    download = 0;   // cb_disk_compressed (0 = unknown -> omit)
	};

	// Build a minimal appinfo "depots" map for a token-locked app whose
	// product-info carries no depots.  For each depot with gid != 0:
	//
	//   body["depots"]["<id>"]["manifests"]["public"]["gid"]      = "<gid>"
	//   body["depots"]["<id>"]["manifests"]["public"]["size"]     = "<size>"     (if > 0)
	//   body["depots"]["<id>"]["manifests"]["public"]["download"] = "<download>" (if > 0)
	//   body["depots"]["<id>"]["config"]["oslist"]                = oslist       (if non-empty)
	//
	// size/download drive Steam's install-dialog size estimate; omitting
	// them leaves the dialog showing "0 B".  Returns the number of depots
	// added.  No-op (returns 0) when `body` already carries a non-empty
	// "depots" map, so a real product-info document is never clobbered.
	inline int injectSynthesizedDepots(YAML::Node& body,
	                                   const std::vector<SynthDepot>& depots)
	{
		if (!body.IsMap()) return 0;

		// Never overwrite real product-info depots.
		if (YAML::Node existing = body["depots"];
		    existing && existing.IsMap() && existing.size() > 0)
			return 0;

		YAML::Node depotsNode(YAML::NodeType::Map);
		int added = 0;
		for (const auto& d : depots)
		{
			if (d.gid == 0) continue; // no archived manifest -> can't plan it
			const std::string key = std::to_string(d.depotId);
			YAML::Node entry(YAML::NodeType::Map);
			YAML::Node pub(YAML::NodeType::Map);
			pub["gid"] = std::to_string(d.gid);
			if (d.size > 0)     pub["size"]     = std::to_string(d.size);
			if (d.download > 0) pub["download"] = std::to_string(d.download);
			entry["manifests"]["public"] = pub;
			if (!d.oslist.empty())
				entry["config"]["oslist"] = d.oslist;
			depotsNode[key] = entry;
			++added;
		}

		if (added > 0) body["depots"] = depotsNode;
		return added;
	}

	// Detect a depot's target OS from its parsed file list (NOT raw bytes —
	// manifest chunk data is full of 0x5C/backslash noise, which made a
	// raw-byte heuristic mislabel every depot "windows").  Precedence:
	// windows (.exe) > macos (.app/.dylib/Contents/MacOS) > linux
	// (.so / -linux-gnu / .x86_64).  Returns "" for a platform-agnostic
	// depot (shared data), which Steam mounts on any OS.
	inline std::string detectOsFromFiles(const std::vector<std::string>& files)
	{
		auto lower = [](std::string s)
		{
			for (auto& c : s)
				c = static_cast<char>(
					Ascii::toLower(static_cast<unsigned char>(c)));
			return s;
		};
		auto ends = [](const std::string& s, const std::string& suf)
		{
			return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
		};

		bool win = false, mac = false, lin = false;
		for (const auto& raw : files)
		{
			const std::string f = lower(raw);
			if (ends(f, ".exe")) win = true;
			if (f.find(".app/") != std::string::npos ||
			    f.find(".app\\") != std::string::npos ||
			    f.find("contents/macos") != std::string::npos ||
			    f.find("contents\\macos") != std::string::npos ||
			    ends(f, ".dylib")) mac = true;
			if (ends(f, ".so") || f.find(".so.") != std::string::npos ||
			    f.find("-linux-gnu") != std::string::npos ||
			    f.find("-linux/") != std::string::npos ||
			    ends(f, ".x86_64") || ends(f, ".x86")) lin = true;
		}
		if (win) return "windows";
		if (mac) return "macos";
		if (lin) return "linux";
		return "";
	}

	// Extract a depot's total sizes from its on-disk manifest so they can be
	// emitted into the synthesized appinfo (without them Steam's install
	// dialog shows "0 B").  A Steam depot manifest is a sequence of
	// <uint32 magic LE><uint32 len LE><payload> sections; the
	// ContentManifestMetadata section (magic 0x1F4812BE) carries
	// cb_disk_original (proto field 5, uncompressed = "size") and
	// cb_disk_compressed (field 6, compressed = "download") as varints.
	//
	// Returns true and fills `sizeOut`/`downloadOut` when the metadata
	// section is found; false otherwise (caller omits the size hints).
	// Every length is bounds-checked: the manifest is local but still
	// treated as untrusted (fail closed, never over-read).
	inline bool parseManifestSizes(const std::string& m,
	                               uint64_t& sizeOut, uint64_t& downloadOut)
	{
		constexpr uint32_t kMetadataMagic = 0x1F4812BEu;

		auto readU32le = [&](size_t off, uint32_t& out) -> bool
		{
			if (off + 4 > m.size()) return false;
			out = static_cast<uint8_t>(m[off]) |
			      (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 1])) << 8) |
			      (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 2])) << 16) |
			      (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 3])) << 24);
			return true;
		};

		// Locate the metadata section by walking the section framing.
		size_t meta = std::string::npos, metaLen = 0;
		for (size_t i = 0; i + 8 <= m.size();)
		{
			uint32_t magic = 0, len = 0;
			if (!readU32le(i, magic) || !readU32le(i + 4, len)) break;
			if (static_cast<uint64_t>(i) + 8 + len > m.size()) break; // truncated
			if (magic == kMetadataMagic) { meta = i + 8; metaLen = len; break; }
			i += 8 + len;
		}
		if (meta == std::string::npos) return false;

		auto readVarint = [&](size_t& p, size_t end, uint64_t& out) -> bool
		{
			uint64_t r = 0; int shift = 0;
			while (p < end)
			{
				const uint8_t b = static_cast<uint8_t>(m[p++]);
				if (shift <= 63) r |= static_cast<uint64_t>(b & 0x7F) << shift;
				shift += 7;
				if (!(b & 0x80)) { out = r; return true; }
				if (shift > 70) return false; // malformed
			}
			return false;
		};

		const size_t end = meta + metaLen;
		bool gotSize = false, gotDownload = false;
		for (size_t p = meta; p < end;)
		{
			uint64_t tag = 0;
			if (!readVarint(p, end, tag)) break;
			const uint32_t field = static_cast<uint32_t>(tag >> 3);
			const uint32_t wire  = static_cast<uint32_t>(tag & 0x7);
			if (wire == 0) // varint
			{
				uint64_t v = 0;
				if (!readVarint(p, end, v)) break;
				if (field == 5) { sizeOut = v; gotSize = true; }
				else if (field == 6) { downloadOut = v; gotDownload = true; }
			}
			else if (wire == 2) // length-delimited: skip
			{
				uint64_t len = 0;
				if (!readVarint(p, end, len) || p + len > end) break;
				p += len;
			}
			else if (wire == 5) { p += 4; }
			else if (wire == 1) { p += 8; }
			else break; // unknown wire type
		}
		return gotSize || gotDownload;
	}

	// Ensure the appinfo body carries a config.installdir — the folder under
	// steamapps/common Steam creates for the title.  A token-locked app's
	// product-info has no `config` block, so without this Steam fails the
	// install with "Invalid install path".  Derives the dir from
	// common.name (Valve's installdir for these titles matches the display
	// name), stripping path-hostile characters.  No-op (returns false) when
	// config.installdir already exists or there is no common.name to use.
	inline bool ensureInstallDir(YAML::Node& body)
	{
		if (!body.IsMap()) return false;

		if (YAML::Node cfg = body["config"]; cfg && cfg.IsMap())
		{
			YAML::Node dir = cfg["installdir"];
			if (dir && dir.IsScalar() && !dir.as<std::string>().empty())
				return false; // real installdir present — keep it
		}

		YAML::Node common = body["common"];
		if (!common || !common.IsMap()) return false;
		YAML::Node name = common["name"];
		if (!name || !name.IsScalar()) return false;

		std::string raw;
		try { raw = name.as<std::string>(); }
		catch (...) { return false; }

		// Replace characters that are invalid in a path component with a
		// space, collapse runs of whitespace, and trim.
		std::string out;
		out.reserve(raw.size());
		bool pendingSpace = false;
		for (unsigned char c : raw)
		{
			const bool bad = c < 0x20 || c == '/' || c == '\\' || c == ':' ||
			                 c == '*' || c == '?' || c == '"' || c == '<' ||
			                 c == '>' || c == '|';
			if (bad || c == ' ')
			{
				if (!out.empty()) pendingSpace = true;
				continue;
			}
			if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
			out.push_back(static_cast<char>(c));
		}
		if (out.empty()) return false;

		body["config"]["installdir"] = out;
		return true;
	}

	// Extract all file paths recorded in a depot manifest's
	// ContentManifestPayload (section magic 0x71F617D0).  The payload is a
	// protobuf: repeated FileMapping mappings = 1, each FileMapping with
	// string filename = 1.  Proper length-delimited parsing (not a byte
	// scan) so long names aren't corrupted by their length prefix.  Returns
	// filenames verbatim (Windows depots use backslash separators); empty if
	// the section is absent or filenames are encrypted.
	inline std::vector<std::string> extractManifestFilenames(const std::string& m)
	{
		constexpr uint32_t kPayloadMagic = 0x71F617D0u;
		std::vector<std::string> out;

		auto u32le = [&](size_t off, uint32_t& v) -> bool
		{
			if (off + 4 > m.size()) return false;
			v = static_cast<uint8_t>(m[off]) |
			    (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 1])) << 8) |
			    (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 2])) << 16) |
			    (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 3])) << 24);
			return true;
		};

		size_t pl = std::string::npos, plLen = 0;
		for (size_t i = 0; i + 8 <= m.size();)
		{
			uint32_t magic = 0, len = 0;
			if (!u32le(i, magic) || !u32le(i + 4, len)) break;
			if (static_cast<uint64_t>(i) + 8 + len > m.size()) break;
			if (magic == kPayloadMagic) { pl = i + 8; plLen = len; break; }
			i += 8 + len;
		}
		if (pl == std::string::npos) return out;

		auto varint = [&](size_t& p, size_t end, uint64_t& v) -> bool
		{
			uint64_t r = 0; int s = 0;
			while (p < end)
			{
				const uint8_t b = static_cast<uint8_t>(m[p++]);
				if (s <= 63) r |= static_cast<uint64_t>(b & 0x7F) << s;
				s += 7;
				if (!(b & 0x80)) { v = r; return true; }
				if (s > 70) return false;
			}
			return false;
		};

		const size_t plEnd = pl + plLen;
		for (size_t p = pl; p < plEnd;)
		{
			uint64_t tag = 0;
			if (!varint(p, plEnd, tag)) break;
			const uint32_t field = static_cast<uint32_t>(tag >> 3);
			const uint32_t wire  = static_cast<uint32_t>(tag & 0x7);
			if (wire == 2)
			{
				uint64_t len = 0;
				if (!varint(p, plEnd, len) || p + len > plEnd) break;
				if (field == 1) // a FileMapping submessage
				{
					// First field of FileMapping is the filename (1, wire 2).
					size_t q = p, qEnd = p + len;
					uint64_t ftag = 0;
					if (varint(q, qEnd, ftag) && (ftag >> 3) == 1 && (ftag & 7) == 2)
					{
						uint64_t flen = 0;
						if (varint(q, qEnd, flen) && q + flen <= qEnd)
							out.emplace_back(m.substr(q, flen));
					}
				}
				p += len;
			}
			else if (wire == 0) { uint64_t v; if (!varint(p, plEnd, v)) break; }
			else if (wire == 5) { p += 4; }
			else if (wire == 1) { p += 8; }
			else break;
		}
		return out;
	}

	// Choose the launch executable for a given OS from a manifest's file
	// list.  Returns "" if none found.
	//   windows -> a ROOT-level ".exe" (prefer stem == installdir, skip
	//              crash handlers / redist installers)
	//   linux   -> a ROOT-level executable: no extension, or .sh/.x86_64/.x86
	//              (prefer name == installdir; skip libraries/data)
	//   macos   -> the "<name>.app" bundle path (derived from any file inside
	//              it; prefer the bundle whose stem matches installdir)
	inline std::string pickLauncher(const std::vector<std::string>& files,
	                                const std::string& installdir,
	                                const std::string& os)
	{
		auto lower = [](std::string s)
		{
			for (auto& c : s)
				c = static_cast<char>(
					Ascii::toLower(static_cast<unsigned char>(c)));
			return s;
		};
		auto ends = [&](const std::string& s, const std::string& suf)
		{
			std::string ls = lower(s), lsuf = lower(suf);
			return ls.size() >= lsuf.size() &&
			       ls.compare(ls.size() - lsuf.size(), lsuf.size(), lsuf) == 0;
		};
		auto isRoot = [](const std::string& f)
		{
			return f.find('\\') == std::string::npos && f.find('/') == std::string::npos;
		};
		auto isHelper = [&](const std::string& f)
		{
			const std::string l = lower(f);
			return l.find("crashhandler") != std::string::npos ||
			       l.find("crashpad") != std::string::npos ||
			       l.find("unitycrash") != std::string::npos ||
			       l.find("vc_redist") != std::string::npos ||
			       l.find("vcredist") != std::string::npos ||
			       l.find("dxsetup") != std::string::npos ||
			       l.find("uninstall") != std::string::npos;
		};
		auto stem = [&](std::string f)
		{
			const auto dot = f.rfind('.');
			if (dot != std::string::npos) f = f.substr(0, dot);
			return lower(f);
		};
		const std::string want = lower(installdir);

		if (os == "macos")
		{
			// The .app is a directory; the manifest lists files inside it.
			// Recover the bundle path prefix "<...>.app".
			std::vector<std::string> bundles;
			for (const auto& f : files)
			{
				const std::string lf = lower(f);
				size_t pos = lf.find(".app/");
				if (pos == std::string::npos) pos = lf.find(".app\\");
				if (pos == std::string::npos) continue;
				const std::string bundle = f.substr(0, pos + 4); // include ".app"
				if (isRoot(bundle)) bundles.push_back(bundle);
			}
			if (bundles.empty()) return "";
			for (const auto& b : bundles)
				if (stem(b) == want) return b;
			return bundles.front();
		}

		std::vector<std::string> cands;
		for (const auto& f : files)
		{
			if (!isRoot(f) || isHelper(f)) continue;
			if (os == "windows")
			{
				if (ends(f, ".exe")) cands.push_back(f);
			}
			else // linux
			{
				const bool lib = ends(f, ".so") || f.find(".so.") != std::string::npos ||
				                 ends(f, ".dll");
				if (lib) continue;
				const bool execLike = ends(f, ".sh") || ends(f, ".x86_64") ||
				                      ends(f, ".x86") || ends(f, ".run") ||
				                      f.find('.') == std::string::npos; // no extension
				if (execLike) cands.push_back(f);
			}
		}
		if (cands.empty()) return "";

		// Prefer the candidate whose stem matches the install dir.
		for (const auto& f : cands)
			if (stem(f) == want) return f;
		// Then a name match ignoring spaces (e.g. "Risk of Rain 2" vs "riskofrain2").
		auto squash = [&](std::string s)
		{
			std::string o; for (char c : lower(s)) if (c != ' ') o.push_back(c); return o;
		};
		const std::string wantSquash = squash(installdir);
		for (const auto& f : cands)
			if (squash(stem(f)) == wantSquash) return f;
		return cands.front();
	}

	// Add a config.launch entry per (executable, oslist) pair.  A
	// token-locked app's product-info has no config.launch, so Steam refuses
	// to start it ("Invalid game configuration"); we synthesize one launch
	// option per OS we found a launcher for, so the game runs natively on its
	// own platform AND through a forced Proton/compat tool (the windows
	// entry).  No-op (returns 0) when a launch block already exists.  Entries
	// with an empty executable are skipped.
	inline int ensureLaunchEntries(YAML::Node& body,
	    const std::vector<std::pair<std::string, std::string>>& entries)
	{
		if (!body.IsMap()) return 0;

		if (YAML::Node cfg = body["config"]; cfg && cfg.IsMap())
		{
			YAML::Node launch = cfg["launch"];
			if (launch && launch.IsMap() && launch.size() > 0)
				return 0; // real launch entries present
		}

		std::string desc;
		if (YAML::Node n = body["common"]["name"]; n && n.IsScalar())
			desc = n.as<std::string>();

		int added = 0;
		for (const auto& [executable, oslist] : entries)
		{
			if (executable.empty()) continue;
			YAML::Node entry(YAML::NodeType::Map);
			entry["executable"] = executable;
			if (!desc.empty()) entry["description"] = desc;
			if (!oslist.empty()) entry["config"]["oslist"] = oslist;
			body["config"]["launch"][std::to_string(added)] = entry;
			++added;
		}
		return added;
	}
}
