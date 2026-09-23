// SPDX-License-Identifier: AGPL-3.0-only
//
// See appinfo_vdf.hpp for design notes and file format.

#include "appinfo_vdf.hpp"
#include "appinfo_provision.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/process_lock.hpp"

#include "base64/base64.hpp"
#include "prewarm.hpp"
#include "provision_cache.hpp"
#include "synthmark.hpp"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <openssl/sha.h>
#include <dlfcn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <ctime>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace AppInfoVdf
{

namespace
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint32_t MAGIC_V41 = 0x07564429;
constexpr uint32_t UNIVERSE_PUBLIC = 1;

#ifdef APPINFO_VDF_TESTING
std::function<void()>& beforeScopedPublishHook()
{
	static std::function<void()> hook;
	return hook;
}
#endif

bool parsePicsBufferName(const std::string& name, uint32_t& appId)
{
	constexpr std::string_view prefix = "picsbuffer_";
	constexpr std::string_view suffix = ".yaml";
	if (name.size() <= prefix.size() + suffix.size()) return false;
	if (name.compare(0, prefix.size(), prefix) != 0) return false;
	if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
		return false;

	const char* first = name.data() + prefix.size();
	const char* last = name.data() + name.size() - suffix.size();
	uint32_t parsed = 0;
	const auto result = std::from_chars(first, last, parsed);
	if (result.ec != std::errc{} || result.ptr != last) return false;
	appId = parsed;
	return true;
}

// KV1 binary node types.
namespace KV
{
	constexpr uint8_t ChildObject     = 0;
	constexpr uint8_t String          = 1;
	constexpr uint8_t Int32           = 2;
	constexpr uint8_t Float32         = 3;
	constexpr uint8_t Pointer         = 4;
	constexpr uint8_t WideString      = 5;
	constexpr uint8_t Color           = 6;
	constexpr uint8_t UInt64          = 7;
	constexpr uint8_t End             = 8;
	constexpr uint8_t Int64           = 10;
	constexpr uint8_t AlternateEnd    = 11;
}

// ---------------------------------------------------------------------------
// Little-endian byte primitives
// ---------------------------------------------------------------------------

template <typename T>
T readLE(const uint8_t* p)
{
	T v;
	std::memcpy(&v, p, sizeof(T));
	return v;
}

template <typename T>
void writeLE(std::vector<uint8_t>& out, T v)
{
	const auto* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + sizeof(T));
}

void writeBytes(std::vector<uint8_t>& out, const void* data, size_t n)
{
	const auto* p = static_cast<const uint8_t*>(data);
	out.insert(out.end(), p, p + n);
}

// ---------------------------------------------------------------------------
// Parsed-file representation
// ---------------------------------------------------------------------------

struct AppEntry
{
	uint32_t appid          = 0;
	uint32_t info_state     = 2;     // normal
	uint32_t last_updated   = 0;
	uint64_t pics_token     = 0;
	uint8_t  sha[20]        = {0};
	uint32_t change_number  = 0;
	uint8_t  binary_hash[20] = {0};
	std::vector<uint8_t> binary_vdf; // v41-indexed (keys are u32 indices)
};

struct AppInfoFile
{
	uint32_t universe = UNIVERSE_PUBLIC;

	std::vector<AppEntry> apps;

	// String table (in original order; index 0 == strings[0]).
	std::vector<std::string> strings;
	std::unordered_map<std::string, uint32_t> stringIndex;

	uint32_t internStringIndex(const std::string& s)
	{
		auto it = stringIndex.find(s);
		if (it != stringIndex.end()) return it->second;
		const auto idx = static_cast<uint32_t>(strings.size());
		strings.push_back(s);
		stringIndex.emplace(s, idx);
		return idx;
	}
};

// ---------------------------------------------------------------------------
// Parse v41 file
// ---------------------------------------------------------------------------

bool readV41(const std::string& path, AppInfoFile& out, std::string& err)
{
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs.is_open())
	{
		err = "cannot open " + path;
		return false;
	}
	const auto fileSize = static_cast<size_t>(ifs.tellg());
	ifs.seekg(0, std::ios::beg);
	std::vector<uint8_t> buf(fileSize);
	ifs.read(reinterpret_cast<char*>(buf.data()),
	         static_cast<std::streamsize>(fileSize));
	if (!ifs.good() || static_cast<size_t>(ifs.gcount()) != fileSize)
	{
		err = "short read";
		return false;
	}

	// Header: u32 magic, u32 universe, i64 string_table_offset
	if (fileSize < 16)
	{
		err = "file too small";
		return false;
	}
	const uint32_t magic = readLE<uint32_t>(buf.data());
	if (magic != MAGIC_V41)
	{
		err = "unsupported magic (only v41 supported)";
		return false;
	}
	out.universe = readLE<uint32_t>(buf.data() + 4);
	const auto stringTableOffset =
	    static_cast<size_t>(readLE<int64_t>(buf.data() + 8));
	if (stringTableOffset < 16 || stringTableOffset > fileSize)
	{
		err = "string table offset out of range";
		return false;
	}

	// Parse string table first (the binary VDF blobs reference it by index).
	{
		const uint8_t* p = buf.data() + stringTableOffset;
		const uint8_t* end = buf.data() + fileSize;
		if (end - p < 4) { err = "string table truncated"; return false; }
		const uint32_t count = readLE<uint32_t>(p);
		p += 4;
		out.strings.reserve(count);
		out.stringIndex.reserve(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint8_t* nul = static_cast<const uint8_t*>(
			    std::memchr(p, 0, static_cast<size_t>(end - p)));
			if (!nul) { err = "string table missing terminator"; return false; }
			std::string s(reinterpret_cast<const char*>(p),
			              static_cast<size_t>(nul - p));
			out.stringIndex.emplace(s, i);
			out.strings.push_back(std::move(s));
			p = nul + 1;
		}
	}

	// Parse apps[] until appid == 0.
	{
		size_t pos = 16;
		while (pos < stringTableOffset)
		{
			if (stringTableOffset - pos < 4)
			{
				err = "truncated before footer";
				return false;
			}
			const uint32_t appid = readLE<uint32_t>(buf.data() + pos);
			pos += 4;
			if (appid == 0) break;

			if (stringTableOffset - pos < 4)
			{
				err = "truncated entry header";
				return false;
			}
			const uint32_t entrySize = readLE<uint32_t>(buf.data() + pos);
			pos += 4;
			if (entrySize > stringTableOffset - pos)
			{
				err = "entry size exceeds remaining bytes";
				return false;
			}
			const size_t entryEnd = pos + entrySize;
			// Header inside entry: info_state, last_updated, token, sha,
			// change_number, binary_hash = 4+4+8+20+4+20 = 60 bytes.
			constexpr size_t headerBytes = 60;
			if (entrySize < headerBytes)
			{
				err = "entry header smaller than expected";
				return false;
			}
			AppEntry e;
			e.appid         = appid;
			e.info_state    = readLE<uint32_t>(buf.data() + pos);     pos += 4;
			e.last_updated  = readLE<uint32_t>(buf.data() + pos);     pos += 4;
			e.pics_token    = readLE<uint64_t>(buf.data() + pos);     pos += 8;
			std::memcpy(e.sha, buf.data() + pos, 20);                  pos += 20;
			e.change_number = readLE<uint32_t>(buf.data() + pos);     pos += 4;
			std::memcpy(e.binary_hash, buf.data() + pos, 20);          pos += 20;

			const size_t vdfBytes = entryEnd - pos;
			e.binary_vdf.assign(buf.data() + pos, buf.data() + pos + vdfBytes);
			pos = entryEnd;
			out.apps.push_back(std::move(e));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Text KeyValues1 -> v41-indexed binary KV
//
// PICS responses deliver KeyValues in *text* format (quoted strings,
// {} blocks, whitespace).  We tokenize the text and emit binary KV1 with
// keys as string-table indices.
//
// The text grammar is small:
//   document := pair*
//   pair    := string string | string '{' pair* '}'
//   string  := '"' (escaped chars)* '"' | bare_word
// We accept '\\' '\"' '\n' '\t' as escapes inside quoted strings.
// ---------------------------------------------------------------------------

class TextLexer
{
public:
	TextLexer(const char* p, const char* end) : p_(p), end_(end) {}

	enum class Tok { String, OpenBrace, CloseBrace, End };

	Tok next(std::string& out)
	{
		out.clear();
		skipWs();
		if (p_ >= end_) return Tok::End;

		const char c = *p_;
		if (c == '{') { ++p_; return Tok::OpenBrace; }
		if (c == '}') { ++p_; return Tok::CloseBrace; }
		if (c == '"') return readQuoted(out);
		return readBare(out);
	}

private:
	void skipWs()
	{
		while (p_ < end_)
		{
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			{
				++p_;
				continue;
			}
			// Skip "// ..." line comments (rare but legal in KV1 text).
			if (c == '/' && p_ + 1 < end_ && p_[1] == '/')
			{
				while (p_ < end_ && *p_ != '\n') ++p_;
				continue;
			}
			break;
		}
	}

	Tok readQuoted(std::string& out)
	{
		++p_; // opening quote
		while (p_ < end_)
		{
			const char c = *p_++;
			if (c == '"') return Tok::String;
			if (c == '\\' && p_ < end_)
			{
				const char e = *p_++;
				switch (e)
				{
					case 'n':  out.push_back('\n'); break;
					case 't':  out.push_back('\t'); break;
					case 'r':  out.push_back('\r'); break;
					case '"':  out.push_back('"');  break;
					case '\\': out.push_back('\\'); break;
					default:   out.push_back(e);    break;
				}
				continue;
			}
			out.push_back(c);
		}
		return Tok::End; // unterminated string treated as EOF
	}

	Tok readBare(std::string& out)
	{
		while (p_ < end_)
		{
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
			    c == '{' || c == '}' || c == '"') break;
			out.push_back(*p_++);
		}
		return Tok::String;
	}

	const char* p_;
	const char* end_;
};

// Try to encode a value as Int32 if it parses cleanly; otherwise emit String.
// PICS appinfo text doesn't tag types, so we mirror what Steam does: read
// every leaf as a string.  The binary KV reader still accepts strings even
// when downstream code expects an int — Steam's appinfo schema uses strings
// pervasively (e.g. depot ids, gid).  Emitting everything as String is the
// safe choice and matches what we see in the original v41 appinfo.vdf:
// numeric-looking values stored as cstrings.
void emitString(std::vector<uint8_t>& out, AppInfoFile& f,
                const std::string& key, const std::string& val)
{
	const uint32_t idx = f.internStringIndex(key);
	out.push_back(KV::String);
	writeLE<uint32_t>(out, idx);
	writeBytes(out, val.data(), val.size());
	out.push_back(0);
}

bool emitObject(TextLexer& lex, AppInfoFile& f,
                std::vector<uint8_t>& out, std::string& err);

// Emit a single pair starting from a key.  Caller has just read `key` as a
// string token; this consumes either another string (-> leaf) or an open
// brace (-> nested object) and writes the pair.
bool emitPair(TextLexer& lex, AppInfoFile& f,
              const std::string& key, std::vector<uint8_t>& out,
              std::string& err)
{
	std::string tok;
	const auto t = lex.next(tok);
	if (t == TextLexer::Tok::String)
	{
		emitString(out, f, key, tok);
		return true;
	}
	if (t == TextLexer::Tok::OpenBrace)
	{
		const uint32_t idx = f.internStringIndex(key);
		out.push_back(KV::ChildObject);
		writeLE<uint32_t>(out, idx);
		if (!emitObject(lex, f, out, err)) return false;
		return true;
	}
	err = "expected value or '{' after key";
	return false;
}

bool emitObject(TextLexer& lex, AppInfoFile& f,
                std::vector<uint8_t>& out, std::string& err)
{
	std::string tok;
	while (true)
	{
		const auto t = lex.next(tok);
		if (t == TextLexer::Tok::CloseBrace)
		{
			out.push_back(KV::End);
			return true;
		}
		if (t == TextLexer::Tok::End)
		{
			err = "unterminated object";
			return false;
		}
		if (t != TextLexer::Tok::String)
		{
			err = "expected key";
			return false;
		}
		if (!emitPair(lex, f, tok, out, err)) return false;
	}
}

bool translateWireToIndexed(const std::string& wire,
                            AppInfoFile& f,
                            std::vector<uint8_t>& outVdf,
                            std::string& err)
{
	if (wire.empty()) { err = "empty wire buffer"; return false; }

	// PICS-wire buffer is null-terminated text.  Trim the trailing null
	// (and any trailing whitespace) before lexing.
	size_t len = wire.size();
	while (len > 0 && (wire[len - 1] == '\0' || wire[len - 1] == '\n' ||
	                   wire[len - 1] == ' '  || wire[len - 1] == '\t' ||
	                   wire[len - 1] == '\r'))
	{
		--len;
	}

	TextLexer lex(wire.data(), wire.data() + len);

	// The PICS buffer is a single top-level pair: "appinfo" { ... }.
	std::string topKey;
	const auto topTok = lex.next(topKey);
	if (topTok != TextLexer::Tok::String)
	{
		err = "wire buffer doesn't start with a key";
		return false;
	}

	if (!emitPair(lex, f, topKey, outVdf, err)) return false;

	// Append the trailing root-level End that v41 binary KV expects.
	outVdf.push_back(KV::End);
	return true;
}

// Validate the exact subset emitted by translateWireToIndexed before it is
// allowed into appinfo.vdf.  This is intentionally strict for synthetic
// entries (ChildObject/String/End only) and deliberately does not reinterpret
// Valve's existing entries, whose binary KV payloads may use additional scalar
// types.  A bad synthetic blob is rejected before it can poison Steam's cache.
bool validateGeneratedObject(const std::vector<uint8_t>& data,
	                         const AppInfoFile& f,
	                         size_t& pos,
	                         unsigned int depth,
	                         std::string& err)
{
	if (depth > 128)
	{
		err = "generated KV nesting is too deep";
		return false;
	}

	while (pos < data.size())
	{
		const uint8_t type = data[pos++];
		if (type == KV::End) return true;
		if (type != KV::ChildObject && type != KV::String)
		{
			err = "generated KV contains an unexpected node type";
			return false;
		}
		if (data.size() - pos < sizeof(uint32_t))
		{
			err = "generated KV key index is truncated";
			return false;
		}
		const uint32_t keyIndex = readLE<uint32_t>(data.data() + pos);
		pos += sizeof(uint32_t);
		if (keyIndex >= f.strings.size())
		{
			err = "generated KV key index is outside the string table";
			return false;
		}

		if (type == KV::ChildObject)
		{
			if (!validateGeneratedObject(data, f, pos, depth + 1, err))
				return false;
			continue;
		}

		const auto* begin = data.data() + pos;
		const auto* end = data.data() + data.size();
		const auto* nul = static_cast<const uint8_t*>(
			std::memchr(begin, 0, static_cast<size_t>(end - begin)));
		if (!nul)
		{
			err = "generated KV string is not NUL-terminated";
			return false;
		}
		pos += static_cast<size_t>(nul - begin) + 1;
	}

	err = "generated KV object is missing its end marker";
	return false;
}

bool validateGeneratedVdf(const std::vector<uint8_t>& data,
	                      const AppInfoFile& f,
	                      std::string& err)
{
	if (data.empty())
	{
		err = "generated KV is empty";
		return false;
	}
	size_t pos = 0;
	if (!validateGeneratedObject(data, f, pos, 0, err)) return false;
	if (pos != data.size())
	{
		err = "generated KV has trailing bytes";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Compute SHA-1 (binary_hash field)
// ---------------------------------------------------------------------------

void sha1(const void* data, size_t n, uint8_t out[20])
{
	static unsigned char* (*p_SHA1)(const unsigned char *d, size_t n, unsigned char *md) = nullptr;
	if (!p_SHA1)
	{
		void* handle = dlopen("libcrypto.so.1.1", RTLD_NOLOAD | RTLD_LAZY);
		if (!handle) handle = dlopen("libcrypto.so.1.0.0", RTLD_NOLOAD | RTLD_LAZY);
		if (!handle) handle = dlopen("libcrypto.so.3", RTLD_NOLOAD | RTLD_LAZY);
		if (!handle) handle = RTLD_DEFAULT;
		p_SHA1 = (unsigned char*(*)(const unsigned char*, size_t, unsigned char*))dlsym(handle, "SHA1");
	}
	
	if (p_SHA1)
		p_SHA1(static_cast<const uint8_t*>(data), n, out);
	else
		memset(out, 0, 20);
}

// ---------------------------------------------------------------------------
// Serialise the parsed AppInfoFile back to disk
// ---------------------------------------------------------------------------

bool serializeV41(const AppInfoFile& f, std::vector<uint8_t>& out)
{
	std::vector<uint8_t> body;
	body.reserve(2 * 1024 * 1024);

	// Apps section
	for (const auto& e : f.apps)
	{
		writeLE<uint32_t>(body, e.appid);

		// `size` placeholder; we'll patch it up after we know the body.
		const size_t sizePos = body.size();
		writeLE<uint32_t>(body, 0);
		const size_t entryStart = body.size();

		writeLE<uint32_t>(body, e.info_state);
		writeLE<uint32_t>(body, e.last_updated);
		writeLE<uint64_t>(body, e.pics_token);
		writeBytes(body, e.sha, 20);
		writeLE<uint32_t>(body, e.change_number);
		writeBytes(body, e.binary_hash, 20);
		writeBytes(body, e.binary_vdf.data(), e.binary_vdf.size());

		const uint32_t entrySize =
		    static_cast<uint32_t>(body.size() - entryStart);
		// Patch the size field in place.
		std::memcpy(body.data() + sizePos, &entrySize, 4);
	}
	// Footer
	writeLE<uint32_t>(body, 0);

	// Now we know the string table offset = 16 (header) + body.size()
	const int64_t stringTableOffset =
	    static_cast<int64_t>(16) + static_cast<int64_t>(body.size());

	// Build the final byte vector: header + body + string table
	out.clear();
	out.reserve(body.size() + 64 * 1024);
	writeLE<uint32_t>(out, MAGIC_V41);
	writeLE<uint32_t>(out, f.universe);
	writeLE<int64_t>(out, stringTableOffset);
	writeBytes(out, body.data(), body.size());

	writeLE<uint32_t>(out, static_cast<uint32_t>(f.strings.size()));
	for (const auto& s : f.strings)
	{
		writeBytes(out, s.data(), s.size());
		out.push_back(0);
	}

	return true;
}

bool writeV41(const std::string& path, const AppInfoFile& f, std::string& err)
{
	std::vector<uint8_t> out;
	if (!serializeV41(f, out)) return false;
	// Write to a unique temporary inode, fsync it, and publish with rename.
	// This also prevents two setup namespaces from sharing the old fixed
	// `appinfo.vdf.tmp` pathname.
	return AtomicFile::write(path,
		reinterpret_cast<const char*>(out.data()), out.size(), err);
}

// ---------------------------------------------------------------------------
// High-level inject
// ---------------------------------------------------------------------------

bool mergeAppImpl(AppInfoFile& f,
                  uint32_t appid,
                  uint32_t changeNumber,
                  const std::string& sha,
                  const std::string& wireBuffer,
                  bool& changed,
                  std::string& err)
{
	changed = false;
	if (sha.size() != 20)
	{
		err = "sha must be 20 bytes";
		return false;
	}
	if (wireBuffer.empty())
	{
		err = "wire buffer empty";
		return false;
	}

	// Idempotency: same appid + same change_number + same sha -> noop.
	for (const auto& e : f.apps)
	{
		if (e.appid == appid && e.change_number == changeNumber &&
		    std::memcmp(e.sha, sha.data(), 20) == 0)
		{
			g_pLog->debug("AppInfoVdf: app=%u already up-to-date "
			              "(change=%u), skipping\n", appid, changeNumber);
			return true;
		}
	}

	// Translate v39-wire -> v41-indexed.
	std::vector<uint8_t> indexed;
	if (!translateWireToIndexed(wireBuffer, f, indexed, err))
	{
		g_pLog->warn("AppInfoVdf: translateWireToIndexed app=%u failed: %s\n",
		             appid, err.c_str());
		return false;
	}
	if (!validateGeneratedVdf(indexed, f, err))
	{
		g_pLog->warn("AppInfoVdf: generated app=%u failed KV validation: %s\n",
		             appid, err.c_str());
		return false;
	}

	// Diagnostic: retain enough detail to correlate a rejected cache without
	// making normal startup logs look like an error stream.
	{
		std::string hex;
		for (size_t i = 0; i < std::min<size_t>(indexed.size(), 24u); ++i)
		{
			char b[4]; std::snprintf(b, sizeof(b), "%02x ", indexed[i]);
			hex += b;
		}
		const uint32_t idxAppinfo = !indexed.empty() && indexed.size() >= 5
		    ? *reinterpret_cast<const uint32_t*>(indexed.data() + 1) : 0u;
		g_pLog->debug("AppInfoVdf: indexed blob app=%u: %s "
		             "(appinfo idx=%u, table size=%zu, wire %zu->indexed %zu)\n",
		             appid, hex.c_str(), idxAppinfo, f.strings.size(),
		             wireBuffer.size(), indexed.size());
	}

	// Build the new entry.
	AppEntry ne;
	ne.appid         = appid;
	ne.info_state    = 2;  // normal
	ne.last_updated  = 0;
	ne.pics_token    = 0;
	std::memcpy(ne.sha, sha.data(), 20);
	ne.change_number = changeNumber;
	sha1(indexed.data(), indexed.size(), ne.binary_hash);
	ne.binary_vdf    = std::move(indexed);

	// Replace existing entry by appid, or append.
	bool replaced = false;
	for (auto& e : f.apps)
	{
		if (e.appid == appid)
		{
			e = std::move(ne);
			replaced = true;
			break;
		}
	}
	if (!replaced)
	{
		f.apps.push_back(std::move(ne));
	}
	changed = true;

	g_pLog->debug("AppInfoVdf: injected app=%u change=%u (%s)\n",
	              appid, changeNumber, replaced ? "replaced" : "appended");
	return true;
}

// ---------------------------------------------------------------------------
// Cache loading helpers (mirror feats/pics.cpp)
// ---------------------------------------------------------------------------

struct CachedBuffer
{
	uint32_t    appid          = 0;
	uint32_t    change_number  = 0;
	std::string sha;     // 20 bytes
	std::string buffer;  // raw v39-inline binary VDF
};

bool loadCachedBuffer(const std::string& metaPath, uint32_t expectedAppId,
                      CachedBuffer& out,
                      std::string& err)
{
	try
	{
		if (!AppInfoProvision::cacheMarkerAllowsRead(expectedAppId))
		{
			err = "cache read invalidated or synthetic marker state is inconsistent";
			return false;
		}
		auto node = YAML::LoadFile(metaPath);
		out.appid         = node["appid"].as<uint32_t>();
		out.change_number = node["change_number"].as<uint32_t>();
		out.sha           = std::string(
		    base64::from_base64(node["sha_b64"].as<std::string>()));
		const auto wireSize = node["wire_size"].as<size_t>();
		const bool markerPresent = SynthMark::isMarked(
		    std::filesystem::path(metaPath).parent_path().string(), expectedAppId);
		const YAML::Node syntheticNode = node["synthetic"];
		const bool hasSyntheticMetadata = syntheticNode.IsDefined();
		const bool synthetic = hasSyntheticMetadata
		    ? syntheticNode.as<bool>()
		    : false;
		if (!AppInfoProvision::cache::syntheticMarkerStateConsistent(
		        hasSyntheticMetadata, synthetic, markerPresent))
		{
			err = "synthetic marker state is inconsistent";
			return false;
		}

		std::string bufPath = metaPath;
		const auto pos = bufPath.rfind(".yaml");
		if (pos == std::string::npos) { err = "bad meta path"; return false; }
		bufPath.replace(pos, 5, ".bin");

		std::ifstream ifs(bufPath, std::ios::binary | std::ios::ate);
		if (!ifs.is_open()) { err = "cannot open " + bufPath; return false; }
		const auto sz = static_cast<size_t>(ifs.tellg());
		if (sz != wireSize)
		{
			err = "wire_size mismatch in cache";
			return false;
		}
		ifs.seekg(0, std::ios::beg);
		out.buffer.resize(sz);
		if (!ifs.read(out.buffer.data(), static_cast<std::streamsize>(sz)))
		{
			err = "buffer read failed";
			return false;
		}

		uint8_t digest[20]{};
		sha1(out.buffer.data(), out.buffer.size(), digest);
		const bool shaMatches = out.sha.size() == sizeof(digest) &&
		    std::memcmp(out.sha.data(), digest, sizeof(digest)) == 0;
		const AppInfoProvision::cache::CacheRecordFacts facts{
		    .requestedAppId = expectedAppId,
		    .metadataAppId = out.appid,
		    .declaredSize = wireSize,
		    .actualSize = out.buffer.size(),
		    .shaSize = out.sha.size(),
		    .shaMatches = shaMatches,
		    .parsed = out.buffer.find("\"appinfo\"") != std::string::npos,
		    .hasUsableContent =
		        !Prewarm::extractDepotsAndGids(out.buffer).empty(),
		};
		if (!AppInfoProvision::cache::isCacheRecordValid(facts))
		{
			err = "metadata, SHA-1, or depot validation failed";
			return false;
		}
		return true;
	}
	catch (const std::exception& e)
	{
		err = e.what();
		return false;
	}
}

std::string appInfoLockPath(const std::string& path)
{
	return path + ".slssteam.lock";
}

std::string rollbackPath(const std::string& path)
{
	return path + ".slssteam-previous";
}

bool hasV41Magic(const std::string& path)
{
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs.is_open()) return false;
	uint8_t bytes[sizeof(uint32_t)]{};
	if (!ifs.read(reinterpret_cast<char*>(bytes), sizeof(bytes))) return false;
	return readLE<uint32_t>(bytes) == MAGIC_V41;
}

std::string recoveryPath(const std::string& path)
{
	return path + ".slssteam-corrupt." +
		std::to_string(static_cast<long long>(std::time(nullptr))) + "." +
		std::to_string(static_cast<long long>(::getpid()));
}

bool quarantineCurrent(const std::string& path)
{
	if (!std::filesystem::exists(path)) return false;
	std::error_code ec;
	const auto target = recoveryPath(path);
	std::filesystem::rename(path, target, ec);
	if (ec)
	{
		g_pLog->warn("AppInfoVdf: cannot quarantine %s: %s\n",
		             path.c_str(), ec.message().c_str());
		return false;
	}
	g_pLog->warn("AppInfoVdf: quarantined invalid v41 cache %s -> %s\n",
	             path.c_str(), target.c_str());
	return true;
}

// Read the file without ever overwriting a structurally invalid cache.  If a
// previous known-good snapshot exists, restore it first; otherwise move the
// invalid v41 file aside so Steam can rebuild it instead of getting stuck in
// its "Loading user data" recovery loop.
bool readWithRecovery(const std::string& path, AppInfoFile& out,
	                  std::string& err)
{
	if (readV41(path, out, err)) return true;
	const std::string originalError = err;
	if (!hasV41Magic(path)) return false;

	const auto previousPath = rollbackPath(path);
	AppInfoFile previous;
	std::string previousError;
	if (readV41(previousPath, previous, previousError))
	{
		std::error_code ec;
		const auto corruptPath = recoveryPath(path);
		std::filesystem::rename(path, corruptPath, ec);
		if (!ec)
		{
			std::filesystem::rename(previousPath, path, ec);
			if (!ec)
			{
				out = std::move(previous);
				g_pLog->warn("AppInfoVdf: restored previous valid cache after parse failure (%s)\n",
				             originalError.c_str());
				return true;
			}
			// Do not leave the main path absent if the restore failed.
			std::error_code restoreEc;
			std::filesystem::rename(corruptPath, path, restoreEc);
		}
	}

	(void)quarantineCurrent(path);
	err = originalError;
	return false;
}

bool snapshotBeforeWrite(const std::string& path, std::string& err)
{
	if (!std::filesystem::exists(path)) return true;
	std::error_code ec;
	std::filesystem::copy_file(path, rollbackPath(path),
		std::filesystem::copy_options::overwrite_existing, ec);
	if (ec)
	{
		err = "cannot create rollback snapshot: " + ec.message();
		return false;
	}
	return true;
}

bool publishChecked(const std::string& path, const AppInfoFile& file,
	               std::string& err)
{
	if (!snapshotBeforeWrite(path, err)) return false;
	if (!writeV41(path, file, err)) return false;

	AppInfoFile verify;
	std::string verifyError;
	if (readV41(path, verify, verifyError)) return true;

	// AtomicFile already guarantees that the visible file is complete.  This
	// second check catches a serializer regression before Steam sees it and
	// restores the last-good snapshot while keeping the failed output for
	// diagnosis.
	(void)quarantineCurrent(path);
	std::error_code ec;
	std::filesystem::rename(rollbackPath(path), path, ec);
	err = "post-write validation failed: " + verifyError;
	if (ec) err += "; rollback failed: " + ec.message();
	return false;
}

bool publishCheckedIfUnchanged(const std::string& path,
	                            const AtomicFile::FileIdentity& expected,
	                            const AppInfoFile& file, std::string& err)
{
	std::vector<uint8_t> serialized;
	if (!serializeV41(file, serialized))
	{
		err = "cannot serialize v41 appinfo";
		return false;
	}
	static std::atomic<unsigned long> validationSequence{0};
	const std::string validationPath = path + ".slssteam-validate." +
		std::to_string(static_cast<long long>(::getpid())) + "." +
		std::to_string(validationSequence.fetch_add(1, std::memory_order_relaxed));
	if (!AtomicFile::write(validationPath,
		reinterpret_cast<const char*>(serialized.data()), serialized.size(), err))
		return false;
	AppInfoFile validation;
	std::string validationError;
	const bool valid = readV41(validationPath, validation, validationError);
	std::error_code cleanupError;
	std::filesystem::remove(validationPath, cleanupError);
	if (!valid)
	{
		err = "pre-publish validation failed: " + validationError;
		return false;
	}
	if (cleanupError)
	{
		err = "cannot remove pre-publish validation file: " +
			cleanupError.message();
		return false;
	}

#ifdef APPINFO_VDF_TESTING
	{
		auto hook = std::move(beforeScopedPublishHook());
		beforeScopedPublishHook() = nullptr;
		if (hook) hook();
	}
#endif

	if (!AtomicFile::writeIfUnchanged(path, expected,
		reinterpret_cast<const char*>(serialized.data()), serialized.size(), err))
		return false;
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string findExistingPath()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	static const char* suffixes[] = {
		"/.steam/steam/appcache/appinfo.vdf",
		"/.steam/debian-installation/appcache/appinfo.vdf",
		"/.local/share/Steam/appcache/appinfo.vdf",
	};
	for (const char* suffix : suffixes)
	{
		const std::string candidate = std::string(home) + suffix;
		if (std::filesystem::exists(candidate)) return candidate;
	}
	return {};
}

bool injectApp(const std::string& path,
               uint32_t appid,
               uint32_t changeNumber,
               const std::string& sha,
               const std::string& wireBuffer)
{
	ProcessLock::FileLock lock(appInfoLockPath(path));
	if (!lock.acquired())
	{
		g_pLog->info("AppInfoVdf: another writer owns %s; skipping app=%u\n",
		              appInfoLockPath(path).c_str(), appid);
		return false;
	}

	std::string err;
	AppInfoFile file;
	if (!readWithRecovery(path, file, err))
	{
		g_pLog->warn("AppInfoVdf: cannot read %s for app=%u: %s\n",
		             path.c_str(), appid, err.c_str());
		return false;
	}

	bool changed = false;
	if (!mergeAppImpl(file, appid, changeNumber, sha, wireBuffer,
	                  changed, err))
	{
		g_pLog->warn("AppInfoVdf: injectApp(%u) failed: %s\n",
		              appid, err.c_str());
		return false;
	}
	if (changed && !publishChecked(path, file, err))
	{
		g_pLog->warn("AppInfoVdf: publish app=%u failed: %s\n",
		             appid, err.c_str());
		return false;
	}
	g_pLog->info("AppInfoVdf: injected app=%u change=%u into %s\n",
	             appid, changeNumber, path.c_str());
	return true;
}

int injectAllCached(const std::string& path)
{
	const auto cacheDir = g_config.getDir() + "/cache";
	if (!std::filesystem::exists(cacheDir)) return 0;

	// Contention is the only reason to stand down. This is the boot splice: it
	// is the single thing that makes a managed app visible on the next start, so
	// an untrustworthy lock path must not silently cost the whole pass. The
	// publish below is conditional on the file identity captured after the read,
	// so proceeding without a trusted lock cannot clobber a competing writer.
	ProcessLock::FileLock lock(appInfoLockPath(path));
	if (lock.heldByAnother())
	{
		g_pLog->info("AppInfoVdf: another writer owns %s; skipping cache splice\n",
		              appInfoLockPath(path).c_str());
		return 0;
	}

	ProcessLock::FileLock cacheLock(AppInfoProvision::cacheLockPath(), false);
	if (cacheLock.heldByAnother())
	{
		g_pLog->info("AppInfoVdf: unable to lock provision cache; skipping cache splice\n");
		return 0;
	}

	AppInfoFile file;
	std::string readError;
	if (!readWithRecovery(path, file, readError))
	{
		g_pLog->warn("AppInfoVdf: refusing to modify %s: %s\n",
		             path.c_str(), readError.c_str());
		return 0;
	}
	AtomicFile::FileIdentity inputIdentity{};
	if (!AtomicFile::readIdentity(path, inputIdentity))
	{
		g_pLog->warn("AppInfoVdf: cannot identify %s for the cache splice\n",
		             path.c_str());
		return 0;
	}

	const auto managed = g_config.managedAppIds.get();
	const auto active = g_config.addedAppIds.get();
	const std::string quarantineSuffix =
		".orphaned." +
		std::to_string(static_cast<long long>(std::time(nullptr))) + "." +
		std::to_string(static_cast<long long>(::getpid()));
	for (const auto& record : SynthMark::quarantineOrphans(
		cacheDir, active, quarantineSuffix))
	{
		g_pLog->infoOnce("AppInfoVdf: quarantined orphan app cache %s -> %s\n",
		                record.original.string().c_str(),
		                record.quarantined.string().c_str());
	}

	struct CachedMetadata
	{
		std::filesystem::path path;
		uint32_t appId = 0;
	};
	std::vector<CachedMetadata> metadata;
	std::error_code iterError;
	for (std::filesystem::directory_iterator it(cacheDir, iterError), end;
	     it != end && !iterError; it.increment(iterError))
	{
		if (!it->is_regular_file()) continue;
		const auto fname = it->path().filename().string();
		uint32_t expectedAppId = 0;
		if (parsePicsBufferName(fname, expectedAppId))
			metadata.push_back({it->path(), expectedAppId});
	}
	std::sort(metadata.begin(), metadata.end(),
	          [](const CachedMetadata& a, const CachedMetadata& b) {
			  return a.path < b.path;
	          });

	int injected = 0;
	bool changed = false;
	for (const auto& record : metadata)
	{
		const auto& metaPath = record.path;
		const auto fname = metaPath.filename().string();
		const uint32_t expectedAppId = record.appId;

		CachedBuffer cb;
		std::string err;
		if (managed.count(expectedAppId) == 0)
		{
			g_pLog->infoOnce("AppInfoVdf: skipping orphan cache app=%u (%s)\n",
			                expectedAppId, fname.c_str());
			continue;
		}
		if (!loadCachedBuffer(metaPath.string(), expectedAppId, cb, err))
		{
			g_pLog->debug("AppInfoVdf: skip %s: %s\n",
			              fname.c_str(), err.c_str());
			continue;
		}
		bool entryChanged = false;
		if (mergeAppImpl(file, cb.appid, cb.change_number, cb.sha, cb.buffer,
		                 entryChanged, err))
		{
			++injected;
			changed = changed || entryChanged;
		}
		else
		{
			g_pLog->debug("AppInfoVdf: skip %s during merge: %s\n",
			              fname.c_str(), err.c_str());
		}
	}
	if (changed)
	{
		// Keep the rollback snapshot readWithRecovery() restores from, then
		// publish conditionally. The unconditional write this replaced was the
		// reason the lock had to be treated as mandatory here.
		std::string writeError;
		if (!snapshotBeforeWrite(path, writeError) ||
			!publishCheckedIfUnchanged(path, inputIdentity, file, writeError))
		{
			g_pLog->warn("AppInfoVdf: transaction aborted after %d entries: %s\n",
			             injected, writeError.c_str());
			return 0;
		}
	}
	if (injected > 0)
	{
		g_pLog->debug("AppInfoVdf: injectAllCached -> %d entries\n", injected);
	}
	return injected;
}

int injectCachedApps(const std::string& path,
	                 const std::unordered_set<uint32_t>& requestedApps)
{
	if (requestedApps.empty()) return 0;

	// Skip only for real contention. An unusable lock path used to abort the
	// whole live splice silently, which left the hot-added app without an
	// appinfo entry until the next Steam restart. The write below is a
	// compare-and-swap on the file identity, so proceeding without a trusted
	// lock cannot corrupt the file: a competing writer changes the identity and
	// the publish aborts cleanly instead.
	ProcessLock::FileLock lock(appInfoLockPath(path));
	if (lock.heldByAnother())
	{
		g_pLog->info("AppInfoVdf: another writer owns %s; skipping live splice\n",
		             appInfoLockPath(path).c_str());
		return 0;
	}
	ProcessLock::FileLock cacheLock(AppInfoProvision::cacheLockPath(), false);
	if (cacheLock.heldByAnother())
	{
		g_pLog->info("AppInfoVdf: provision cache is busy; skipping live splice\n");
		return 0;
	}

	AppInfoFile file;
	std::string readError;
	if (!readWithRecovery(path, file, readError))
	{
		g_pLog->warn("AppInfoVdf: cannot read %s for the live splice: %s\n",
		             path.c_str(), readError.c_str());
		return 0;
	}
	AtomicFile::FileIdentity inputIdentity{};
	if (!AtomicFile::readIdentity(path, inputIdentity))
	{
		g_pLog->warn("AppInfoVdf: cannot identify %s for the live splice\n",
		             path.c_str());
		return 0;
	}

	const auto managed = g_config.managedAppIds.get();
	const auto cacheDir = g_config.getDir() + "/cache";
	int injected = 0;
	bool changed = false;
	for (const uint32_t appId : requestedApps)
	{
		if (appId == 0 || managed.count(appId) == 0) continue;
		const std::string metaPath = cacheDir + "/picsbuffer_" +
			std::to_string(appId) + ".yaml";
		CachedBuffer cb;
		std::string err;
		if (!loadCachedBuffer(metaPath, appId, cb, err))
		{
			g_pLog->warn("AppInfoVdf: live splice rejected cache pair for "
			             "app=%u: %s\n", appId, err.c_str());
			continue;
		}
		bool entryChanged = false;
		if (!mergeAppImpl(file, cb.appid, cb.change_number, cb.sha, cb.buffer,
		                  entryChanged, err))
		{
			g_pLog->warn("AppInfoVdf: live splice could not merge app=%u: %s\n",
			             appId, err.c_str());
			continue;
		}
		++injected;
		changed = changed || entryChanged;
	}
	if (changed)
	{
		std::string writeError;
		if (!publishCheckedIfUnchanged(path, inputIdentity, file, writeError))
		{
			g_pLog->warn("AppInfoVdf: scoped transaction aborted: %s\n",
			             writeError.c_str());
			return 0;
		}
	}
	return injected;
}

int injectValidatedMetadataApps(
	const std::string& path,
	const std::vector<MetadataApp>& metadataApps)

{
	return injectValidatedMetadataAppsGuarded(
		path, metadataApps, nullptr, nullptr);
}

int injectValidatedMetadataAppsGuarded(
	const std::string& path,
	const std::vector<MetadataApp>& metadataApps,
	void* context,
	MetadataCommitGuard guard)
{
	if (metadataApps.empty()) return 0;

	// Contention is the only reason to stand down; the publish below is a
	// compare-and-swap on the file identity, so an untrustworthy lock path does
	// not have to cost us the transaction.
	ProcessLock::FileLock lock(appInfoLockPath(path));
	if (lock.heldByAnother())
	{
		g_pLog->info("AppInfoVdf: another writer owns %s; skipping DLC metadata "
		             "splice\n", appInfoLockPath(path).c_str());
		return 0;
	}
	if (guard != nullptr && !guard(context)) return 0;

	AppInfoFile file;
	std::string error;
	if (!readWithRecovery(path, file, error)) return 0;
	AtomicFile::FileIdentity inputIdentity{};
	if (!AtomicFile::readIdentity(path, inputIdentity)) return 0;

	bool changed = false;
	for (const MetadataApp& app : metadataApps)
	{
		if (app.appid == 0) return 0;
		const bool alreadyPresent = std::any_of(
			file.apps.begin(), file.apps.end(),
			[&app](const AppEntry& entry) { return entry.appid == app.appid; });
		if (alreadyPresent)
		{
			// Never downgrade an entry supplied by Steam (or a previous complete
			// source) to metadata-only data: it may contain owned DLC depots,
			// launch configuration, or tokens that this safe record strips.
			continue;
		}
		bool entryChanged = false;
		if (!mergeAppImpl(file, app.appid, app.changeNumber, app.sha,
		                  app.wireBuffer, entryChanged, error))
			return 0;
		changed = changed || entryChanged;
	}
	if (changed && !publishCheckedIfUnchanged(
		path, inputIdentity, file, error))
	{
		g_pLog->warn("AppInfoVdf: DLC metadata transaction aborted: %s\n",
		             error.c_str());
		return 0;
	}
	return static_cast<int>(metadataApps.size());
}

#ifdef APPINFO_VDF_TESTING
void setBeforeScopedPublishHook(std::function<void()> hook)
{
	beforeScopedPublishHook() = std::move(hook);
}
#endif

} // namespace AppInfoVdf
