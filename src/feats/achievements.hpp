#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

class CProtoBufMsgBase;

namespace Achievements
{
	// Pure, dependency-free gate for the "we just spoofed a stats request
	// for this appid" decision.
	//
	// eMsg 819 (CMsgClientGetUserStatsResponse) carries no jobid to
	// correlate with its 818 request, so the response handler keys off a
	// per-appid record set by the send handler: a spoofed request -> strip
	// the dummy-account stats and force eresult=OK; a pass-through request
	// (Steam already had a cached schema) -> leave the response untouched so
	// Steam keeps its own cache instead of being told the user has 0
	// unlocks. Entries older than the TTL are pruned so the map stays
	// bounded even if a request never gets a matching response.
	//
	// Header-only and Steam/SDK-free so it can be unit-tested with a stock
	// g++, same pattern as feats/dlcids.hpp.
	class SpoofTracker
	{
	public:
		explicit SpoofTracker(uint64_t ttlSeconds = 30) : m_ttl(ttlSeconds) {}

		// Record that appId was just spoofed at nowSeconds, pruning any
		// entries that have aged past the TTL.
		void mark(uint32_t appId, uint64_t nowSeconds)
		{
			prune(nowSeconds);
			m_pending[appId] = nowSeconds;
		}

		// If appId has a live (non-expired) spoof record, erase it and
		// return true; otherwise return false. Prunes stale entries first.
		bool consume(uint32_t appId, uint64_t nowSeconds)
		{
			prune(nowSeconds);
			auto it = m_pending.find(appId);
			if (it == m_pending.end())
			{
				return false;
			}
			m_pending.erase(it);
			return true;
		}

		std::size_t size() const { return m_pending.size(); }

	private:
		void prune(uint64_t nowSeconds)
		{
			for (auto it = m_pending.begin(); it != m_pending.end();)
			{
				// Guard against a clock that appears to move backwards; only
				// forward age beyond the TTL is considered stale.
				const bool stale = nowSeconds > it->second &&
				                   (nowSeconds - it->second) > m_ttl;
				if (stale)
				{
					it = m_pending.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		uint64_t m_ttl;
		std::unordered_map<uint32_t, uint64_t> m_pending; // appId -> markedAt (s)
	};

	// Resolve the owner SteamID64 to impersonate when fetching a game's
	// achievement schema. A per-app override wins; otherwise defaultOwner.
	inline uint64_t resolveOwnerSteamId(
		uint32_t appId,
		const std::unordered_map<uint32_t, uint64_t>& perApp,
		uint64_t defaultOwner)
	{
		const auto it = perApp.find(appId);
		if (it != perApp.end() && it->second != 0)
		{
			return it->second;
		}
		return defaultOwner;
	}

	// Hook entry points (side-effecting; defined in achievements.cpp).
	void sendMessage(CProtoBufMsgBase* msg);
	void recvMessage(const CProtoBufMsgBase* msg);
}
