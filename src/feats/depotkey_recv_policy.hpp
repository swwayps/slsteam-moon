// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure decision for an incoming depot-key response, split out so it carries no
// protobuf / Steam / disk dependency and can be unit-tested standalone
// (tools/test_depotkey_recv_policy.cpp), the same way depotkey_scope.hpp is.
//
// The transport hooks (classic CProtoBufMsgBase and the newer CNetPacket
// CM-receive path) both funnel through classifyRecv so the substitution
// behaviour is identical regardless of which transport delivered the message.

#pragma once

namespace DepotKey
{
	enum class RecvAction
	{
		None,             // leave the response untouched
		CacheObserved,    // Steam served a real key; persist it (no rewrite)
		SubstituteCached, // Steam errored but we hold a key; serve ours
		SynthZero,        // Steam errored, no key, but it's an AddedApp depot
	};

	// eresultOk     - the response carried eresult == OK
	// okKeyIs32     - AND its depot_encryption_key is a valid 32-byte key
	// haveCachedKey - we hold a usable 32-byte key for this depot (error path)
	// isAddedDepot  - the depot id belongs to a managed AddedApp (error path)
	inline RecvAction classifyRecv(bool eresultOk, bool okKeyIs32,
	                               bool haveCachedKey, bool isAddedDepot) noexcept
	{
		if (eresultOk)
		{
			return okKeyIs32 ? RecvAction::CacheObserved : RecvAction::None;
		}
		if (haveCachedKey)
		{
			return RecvAction::SubstituteCached;
		}
		if (isAddedDepot)
		{
			return RecvAction::SynthZero;
		}
		return RecvAction::None;
	}
}
