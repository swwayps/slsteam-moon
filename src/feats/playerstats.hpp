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
// SLSsteam does not use the generated player proto, so these do raw wire
// editing. Pure (no Steam/SDK deps) and unit-testable with a stock g++,
// same pattern as feats/dlcids.hpp.

#pragma once
#include "protowire.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace PlayerStats
{

namespace detail = ::ProtoWire;

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

} // namespace PlayerStats
