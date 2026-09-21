// SPDX-License-Identifier: AGPL-3.0-only
//
// Standalone test for the PURE depot-key receive decision
// (src/feats/depotkey_recv_policy.hpp :: DepotKey::classifyRecv).
//
// This is the decision that was silently dead on the newer Steam client: the
// depot-key response now arrives as a CNetPacket CM frame (eMsg 5439) instead
// of via CProtoBufMsgBase::InitFromPacket, so DepotKey::recvDepotKey never ran
// and Steam bailed with "Missing decryption key".  Both transports now funnel
// through classifyRecv, so pinning its behaviour guards the substitution logic
// independent of which transport delivered the message.
//
// Build (from repo root):
//   g++ -std=c++20 tools/test_depotkey_recv_policy.cpp -o /tmp/test_depotkey_recv_policy && /tmp/test_depotkey_recv_policy

#include "../src/feats/depotkey_recv_policy.hpp"

#include <cassert>
#include <cstdio>

using DepotKey::classifyRecv;
using DepotKey::RecvAction;

int main()
{
	// Steam served a real key (eresult OK + 32-byte key): observe/cache it,
	// never rewrite — regardless of whether we also hold one or it's added.
	assert(classifyRecv(true, true, false, false) == RecvAction::CacheObserved);
	assert(classifyRecv(true, true, true, true) == RecvAction::CacheObserved);

	// eresult OK but the key isn't a usable 32-byte key: leave it alone, don't
	// poison the cache with junk.
	assert(classifyRecv(true, false, false, false) == RecvAction::None);
	assert(classifyRecv(true, false, true, true) == RecvAction::None);

	// Error path: a cached key wins (this is the AVA fix — Steam errored but we
	// hold the managed key), and takes precedence over the zero-key synth.
	assert(classifyRecv(false, false, true, false) == RecvAction::SubstituteCached);
	assert(classifyRecv(false, false, true, true) == RecvAction::SubstituteCached);

	// Error path, no cached key, but the depot belongs to a managed AddedApp:
	// synthesise a zero key so the downloader proceeds.
	assert(classifyRecv(false, false, false, true) == RecvAction::SynthZero);

	// Error path, no cached key, not ours: never touch an owned game / runtime.
	assert(classifyRecv(false, false, false, false) == RecvAction::None);

	// okKeyIs32 is meaningless on the error path and must not leak through.
	assert(classifyRecv(false, true, false, false) == RecvAction::None);

	std::puts("test_depotkey_recv_policy: all assertions passed");
	return 0;
}
