// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure Steam CM websocket wire helpers (no I/O).
//
// These back the native anonymous CM product-info client (feats/cmclient)
// that replaces the external api.steamcmd.net dependency.  Everything here
// is a deterministic byte transform so it can be unit tested on the host
// without a network (tools/test_cmwire.cpp); the transport (TLS, websocket
// handshake, CM server list) lives in cmclient.cpp.
//
// Steam CM packet framing, per websocket binary message:
//   <uint32 emsg (high bit = proto mask 0x80000000)>
//   <uint32 header_len>
//   <CMsgProtoBufHeader bytes>
//   <body protobuf bytes>
//
// Treat every byte off the wire as untrusted: bounds-check each length
// before slicing, and fail closed (return false / empty) on anything
// malformed rather than risk an overrun.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace CmWire
{

// High bit of the emsg field marks a protobuf-framed message.
constexpr uint32_t PROTO_MASK = 0x80000000u;

// Frame a protobuf message into a CM websocket packet.  `emsg` is the
// raw EMsg value (the proto mask is OR'd in here); `hdr` is the
// serialised CMsgProtoBufHeader; `body` is the serialised message body.
inline std::string packPacket(uint32_t emsg, const std::string& hdr,
                              const std::string& body)
{
	std::string out;
	const uint32_t e  = emsg | PROTO_MASK;
	const uint32_t hl = static_cast<uint32_t>(hdr.size());
	out.resize(8);
	std::memcpy(&out[0], &e, 4);
	std::memcpy(&out[4], &hl, 4);
	out += hdr;
	out += body;
	return out;
}

// Parse a CM websocket packet into its emsg (proto mask stripped), header
// bytes, and body bytes.  Returns false on any malformed/truncated input.
inline bool parsePacket(const std::string& d, uint32_t& emsg,
                        std::string& hdr, std::string& body)
{
	if (d.size() < 8) return false;
	uint32_t e = 0, hl = 0;
	std::memcpy(&e, &d[0], 4);
	std::memcpy(&hl, &d[4], 4);
	if (static_cast<uint64_t>(8) + hl > d.size()) return false;
	emsg = e & ~PROTO_MASK;
	hdr  = d.substr(8, hl);
	body = d.substr(8 + hl);
	return true;
}

// Split a decompressed CMsgMulti `message_body` into its inner packets.
// The body is a sequence of <uint32 len><len bytes> records.  Returns the
// inner packets in order, or an empty vector if the stream is malformed
// (a length prefix that would over-read the buffer).
inline std::vector<std::string> expandMultiBody(const std::string& blob)
{
	std::vector<std::string> out;
	size_t off = 0;
	while (off + 4 <= blob.size())
	{
		uint32_t sz = 0;
		std::memcpy(&sz, &blob[off], 4);
		off += 4;
		if (static_cast<uint64_t>(off) + sz > blob.size())
		{
			out.clear();
			break;
		}
		out.emplace_back(blob.substr(off, sz));
		off += sz;
	}
	return out;
}

} // namespace CmWire

// ---------------------------------------------------------------------------
// Protobuf-backed body builders / parsers.
//
// Guarded by CMWIRE_PROTOBUF so the pure framing helpers above (Tasks 1-2)
// stay unit-testable on the host without linking protobuf.  The real
// build (cmclient.cpp) and the `make test-cmwire` target define this.
// ---------------------------------------------------------------------------
#ifdef CMWIRE_PROTOBUF

#include "../sdk/protobufs/steammessages_clientserver_appinfo.pb.h"
#include "../sdk/protobufs/steammessages_clientserver_login.pb.h"

#include <unordered_map>
#include <vector>

namespace CmWire
{

// Anonymous-user logon body.  The header steamid is the anon identity
// ((1<<56)|(10<<52)) and is set by the caller on the CMsgProtoBufHeader;
// these are just the client identity fields the CM expects.
inline std::string buildAnonLogon()
{
	CMsgClientLogon b;
	b.set_protocol_version(65580);
	b.set_client_package_version(1561159470);
	b.set_client_os_type(4); // Linux
	return b.SerializeAsString();
}

// A batched PICS product-info request for the given appids (full data,
// not metadata-only) — one round-trip for the whole AdditionalApps fleet.
inline std::string buildPicsRequest(const std::vector<uint32_t>& appids)
{
	CMsgClientPICSProductInfoRequest r;
	r.set_meta_data_only(false);
	for (uint32_t a : appids)
	{
		auto* e = r.add_apps();
		e->set_appid(a);
	}
	return r.SerializeAsString();
}

// Parse a PICS product-info response body.  Fills `out[appid] = buffer`
// for every app that carries a non-empty wire buffer (the public
// product-info VDF).  When `changesOut` is non-null, also records each
// such app's `change_number` (used as part of the appinfo.vdf
// idempotency key).  Returns true if the CM signalled more responses are
// pending (response_pending) — the caller keeps reading until false.  An
// unreadable body returns false and sets `*malformed`.
inline bool parsePicsResponse(const std::string& body,
                              std::unordered_map<uint32_t, std::string>& out,
                              std::unordered_map<uint32_t, uint32_t>* changesOut = nullptr,
                              bool* malformed = nullptr)
{
	CMsgClientPICSProductInfoResponse r;
	if (!r.ParseFromString(body))
	{
		if (malformed) *malformed = true;
		return false;
	}
	for (const auto& app : r.apps())
	{
		if (app.buffer().empty()) continue;
		out[app.appid()] = app.buffer();
		if (changesOut) (*changesOut)[app.appid()] = app.change_number();
	}
	return r.response_pending();
}

} // namespace CmWire

#endif // CMWIRE_PROTOBUF
