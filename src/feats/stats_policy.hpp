#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

struct CAppOwnershipInfo;

namespace StatsPolicy
{
// Only observations from Steam's ORIGINAL ownership routine are accepted.
// Missing evidence is not a local-only license. Account/license messages
// invalidate passive observations; foreground stats requests query Steam anew.
struct Context { uint32_t account; uint64_t epoch; };
class Store
{
public:
	void setAccount(uint32_t account)
	{
		std::lock_guard lock(m_mutex);
		if (m_account == account) return;
		m_account = account;
		m_apps.clear();
		++m_epoch;
	}
	void invalidate()
	{
		std::lock_guard lock(m_mutex);
		m_apps.clear();
		++m_epoch;
	}
	uint32_t account() const
	{
		std::lock_guard lock(m_mutex);
		return m_account;
	}
	Context context() const
	{
		std::lock_guard lock(m_mutex);
		return {m_account, m_epoch};
	}
	void observe(uint32_t account, uint32_t app, bool success, int32_t package,
	             bool owns, bool expired, uint64_t expectedEpoch)
	{
		std::lock_guard lock(m_mutex);
		if (!account || account != m_account || expectedEpoch != m_epoch || !app) return;
		// A real package always wins, including an expired/borrowed license.
		const bool native = success && package > 0;
		auto& entry = m_apps[app];
		entry.native = entry.native || native;
		entry.local = !entry.native && success && package == 0 && owns && !expired;
	}
	uint64_t localEpoch(uint32_t account, uint32_t app) const
	{
		std::lock_guard lock(m_mutex);
		if (!account || account != m_account) return 0;
		auto it = m_apps.find(app);
		if (it == m_apps.end() || !it->second.local) return 0;
		return m_epoch;
	}
private:
	struct Entry { bool native = false; bool local = false; };
	mutable std::mutex m_mutex;
	uint32_t m_account = 0;
	uint64_t m_epoch = 1;
	std::unordered_map<uint32_t, Entry> m_apps;
};

inline bool isSelf(uint64_t target, uint32_t account)
{
	return account && (!target || target == (0x0110000100000000ULL | account));
}

void setAccount(uint32_t account);
void invalidate();
uint32_t account();
Context context();
void observe(Context context, uint32_t app, bool success, const CAppOwnershipInfo* info);
// refresh=true is ONLY for a Steam-owned request thread. Background consumers
// use the session-scoped observation; they never call Steam from their thread.
uint64_t localEpoch(uint32_t app, uint32_t account, bool refresh = false);
}

// C-only ABI across the LD_AUDIT / LD_PRELOAD namespaces. 0 means passthrough.
extern "C" uint64_t slsteam_local_stats_epoch_v1(uint32_t app, uint32_t account,
                                                uint32_t refresh) noexcept;
