// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure protobuf wire helpers for the unified Player.GetUserStats#1 service
// method (eMsg 151 request / 147 response).
//
// Modern Steam clients fetch a game's achievement schema for the library
// page via Player.GetUserStats#1, not the legacy ClientGetUserStats (818).
// For an AdditionalApp the account doesn't own server-side, that request
// returns empty and the library Achievements tab never appears. The fix
// (mirroring OpenSteamTool/LumaCore) rewrites the outgoing request's steamid
// to a real owner so the server returns a populated schema, and strips the
// owner's per-stat unlock data from the response.
//
//   CPlayer_GetUserStats_Request  { uint64 steamid=1; uint32 appid=2;
//                                   bytes sha_schema=3; uint32 crc_stats=4; }
//   CPlayer_GetUserStats_Response { bytes sha_schema=1; uint32 crc_stats=2;
//                                   bytes schema=3; repeated Stats stats=4; }
//
// The player proto isn't compiled into the fork, so these do raw wire
// editing. Pure (no Steam/SDK deps) and unit-testable with a stock g++,
// same pattern as feats/dlcids.hpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace PlayerStats
{

namespace detail
{

// Read a base-128 varint at `pos` (advancing it). Returns false on overrun
// or an over-long (>10 byte) varint.
inline bool readVarint(const uint8_t* d, std::size_t len, std::size_t& pos, uint64_t& out)
{
	uint64_t result = 0;
	int shift = 0;
	while (pos < len && shift <= 63)
	{
		const uint8_t b = d[pos++];
		result |= static_cast<uint64_t>(b & 0x7F) << shift;
		if (!(b & 0x80))
		{
			out = result;
			return true;
		}
		shift += 7;
	}
	return false;
}

// One top-level field located on the wire.
struct Field
{
	uint32_t    number;
	uint8_t     wireType;
	std::size_t start;     // offset of the tag byte
	std::size_t end;       // one past the field's last byte
	std::size_t valueOff;  // offset of the value (after tag, after len for wt2)
	std::size_t valueLen;  // value length in bytes (wt0: the varint's bytes)
};

// Walk the top-level fields of a protobuf message, invoking `fn(Field)` for
// each. Returns false if the buffer is malformed (caller decides how to cope
// with whatever was emitted so far). Never reads out of bounds.
template <typename Fn>
inline bool walk(const uint8_t* d, std::size_t len, Fn&& fn)
{
	std::size_t pos = 0;
	while (pos < len)
	{
		const std::size_t tagStart = pos;
		uint64_t tag = 0;
		if (!readVarint(d, len, pos, tag)) return false;
		const uint32_t number   = static_cast<uint32_t>(tag >> 3);
		const uint8_t  wireType = static_cast<uint8_t>(tag & 0x7);
		if (number == 0) return false;

		std::size_t valueOff = pos;
		std::size_t valueLen = 0;
		switch (wireType)
		{
		case 0: // varint
		{
			const std::size_t vstart = pos;
			uint64_t tmp = 0;
			if (!readVarint(d, len, pos, tmp)) return false;
			valueOff = vstart;
			valueLen = pos - vstart;
			break;
		}
		case 1: // fixed64
			if (pos + 8 > len) return false;
			valueOff = pos; valueLen = 8; pos += 8;
			break;
		case 5: // fixed32
			if (pos + 4 > len) return false;
			valueOff = pos; valueLen = 4; pos += 4;
			break;
		case 2: // length-delimited
		{
			uint64_t l = 0;
			if (!readVarint(d, len, pos, l)) return false;
			if (pos + l > len) return false;
			valueOff = pos; valueLen = static_cast<std::size_t>(l); pos += valueLen;
			break;
		}
		default: // groups (3,4) and unknown types are unsupported here
			return false;
		}

		fn(Field{ number, wireType, tagStart, pos, valueOff, valueLen });
	}
	return true;
}

inline void putVarint(std::vector<uint8_t>& out, uint64_t v)
{
	while (v >= 0x80) { out.push_back(static_cast<uint8_t>(v) | 0x80); v >>= 7; }
	out.push_back(static_cast<uint8_t>(v));
}

} // namespace detail

// Read the appid (field 2, varint) from a CPlayer_GetUserStats_Request body.
inline std::optional<uint32_t> parseRequestAppId(const uint8_t* d, std::size_t len)
{
	std::optional<uint32_t> found;
	detail::walk(d, len, [&](const detail::Field& f) {
		if (f.number == 2 && f.wireType == 0)
		{
			std::size_t p = f.valueOff;
			uint64_t v = 0;
			if (detail::readVarint(d, len, p, v)) found = static_cast<uint32_t>(v);
		}
	});
	return found;
}

// Read the steamid (field 1, varint) from a request body. Used in tests and
// for diagnostics.
inline std::optional<uint64_t> parseRequestSteamId(const uint8_t* d, std::size_t len)
{
	std::optional<uint64_t> found;
	detail::walk(d, len, [&](const detail::Field& f) {
		if (f.number == 1 && f.wireType == 0)
		{
			std::size_t p = f.valueOff;
			uint64_t v = 0;
			if (detail::readVarint(d, len, p, v)) found = v;
		}
	});
	return found;
}

// True if the message carries a top-level field with the given number.
inline bool hasField(const uint8_t* d, std::size_t len, uint32_t number)
{
	bool present = false;
	detail::walk(d, len, [&](const detail::Field& f) {
		if (f.number == number) present = true;
	});
	return present;
}

// Return the raw bytes of the first length-delimited field with `number`.
inline std::vector<uint8_t> fieldBytes(const uint8_t* d, std::size_t len, uint32_t number)
{
	std::vector<uint8_t> out;
	bool done = false;
	detail::walk(d, len, [&](const detail::Field& f) {
		if (!done && f.number == number && f.wireType == 2)
		{
			out.assign(d + f.valueOff, d + f.valueOff + f.valueLen);
			done = true;
		}
	});
	return out;
}

// Build a minimal CPlayer_GetUserStats_Request carrying only steamid (1) and
// appid (2). Dropping sha_schema (3) and crc_stats (4) forces the server to
// return a full, fresh schema rather than a "no update" reply.
inline std::vector<uint8_t> buildSpoofedRequest(uint64_t steamid, uint32_t appid)
{
	std::vector<uint8_t> out;
	out.push_back(0x08); // field 1, wire type 0
	detail::putVarint(out, steamid);
	out.push_back(0x10); // field 2, wire type 0
	detail::putVarint(out, appid);
	return out;
}

// Copy a CPlayer_GetUserStats_Response, dropping the repeated stats (field 4)
// so the library shows the achievement schema without the impersonated
// owner's unlock progress. Other fields (incl. schema=3) are preserved
// verbatim. On malformed input returns whatever was copied before the fault.
inline std::vector<uint8_t> stripResponseStats(const uint8_t* d, std::size_t len)
{
	std::vector<uint8_t> out;
	detail::walk(d, len, [&](const detail::Field& f) {
		if (f.number == 4) return; // drop stats
		out.insert(out.end(), d + f.start, d + f.end);
	});
	return out;
}

} // namespace PlayerStats
