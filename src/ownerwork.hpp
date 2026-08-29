// SPDX-License-Identifier: AGPL-3.0-only
//
// Owner-IPC-thread execution site for watcher-originated Steam-owned work.
//
// SCOPE — this is HARDENING plus DIAGNOSTICS, not a cumulative-failure fix.
// A controlled VM run proved that the inotify config-watcher pthread executes
// Steam-owned calls itself (the resolved `CUtlMemoryGrow` while appending to
// package 0's live CUtlVector, and Steam's `NotifyLicensesUpdated`) while the
// latched owner thread of `IClientUtils::RunIPCFrame` is a different thread.
// The same run did NOT reproduce any client failure and did NOT observe the
// hypothesised overlap with an in-flight owner frame, so nothing here is
// claimed to fix anything. What it does is remove an unwanted execution site:
// Steam-owned state is mutated from the thread Steam dispatches on.
//
// SEMANTICS ARE PRESERVED. The same calls run with the same inputs in the same
// order; only the thread changes. Every path that cannot use the owner thread
// (no owner latched yet, queue full, queue disabled, teardown in progress)
// falls back to exactly today's behaviour rather than dropping or reordering
// work.
//
// FIRE-AND-FORGET, AND WHY THE BLOCKING WAIT WAS REMOVED
// -----------------------------------------------------
// The first version made the watcher thread wait up to 1500 ms for the owner
// to pick the work up. Guest measurements ruled that out: the owner-thread
// wake cadence on an idle client was ~3.30 s and ~3.41 s (measured twice), and
// adding five extra drain points did not shorten it (3.30 s -> 3.41 s), so
// drain density is not the limiter. At 1500 ms the wait therefore ALWAYS
// expired: the watcher paid 1.5 s of latency and then ran the work off-owner
// anyway — the worst of both options.
//
// So the handoff is now fire-and-forget: `submitHotAdd`/`submitInstallApp`
// enqueue and return (microseconds), and the owner thread drains whenever it
// next runs a frame. The hot-add is already asynchronous from the user's point
// of view, so seconds of handoff latency cost nothing.
//
// WHAT IF THE OWNER NEVER DRAINS?
// -------------------------------
// Two honest options: accept eventual execution (a hot-add could silently
// never apply if the client's IPC stops or the dispatcher thread is replaced),
// or keep a bounded staleness escape hatch. This picks the escape hatch,
// because "the game never appears and nothing is logged" is a worse failure
// than "the call ran on the watcher thread, as it always used to".
//
// The escape hatch is:
//   * threshold `kDefaultMaxStaleMs` (30 s) — ~9x the measured 3.3-3.4 s
//     cadence, so a normally-running client never reaches it;
//   * evaluated by `idleTick()`, which the watcher loop already calls on its
//     bounded poll timeout, plus once per submit — no new thread (spawning
//     threads from the LD_AUDIT preinit path is a recorded anti-pattern);
//   * runs the WHOLE pending set inline, in FIFO order, so total order is
//     preserved;
//   * reported in `~/.SLSsteam.log` and in the trace as `direct_stale`, so it
//     is never silent;
//   * `SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS=0` switches it off, i.e. explicitly
//     chooses "accept eventual execution".
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "feats/hotreload_types.hpp"

namespace OwnerWork
{
	// How long a pending command may sit unclaimed before the escape hatch
	// runs it inline. Well above the measured owner cadence (~3.3-3.4 s on an
	// idle client), so this is reached only when the owner thread has really
	// stopped running IPC frames.
	// Override with SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS (0 = never escape).
	inline constexpr unsigned int kDefaultMaxStaleMs = 30000;
	inline constexpr unsigned int kMaxMaxStaleMs = 600000;

	// Bounded wait the watcher loop uses on its inotify poll, which is also
	// the resolution of the staleness sweep. Small enough that the escape
	// hatch fires promptly once it is due, large enough to be invisible
	// (2 idle wakeups per second per watcher).
	inline constexpr int kIdleTickMs = 500;

	// How a submitted unit of work was dispatched.
	enum class Mode : std::uint8_t
	{
		Queued = 0,  // handed to the owner IPC thread; runs on its next frame
		OnOwner,     // caller already WAS the owner thread; ran inline
		NoOwner,     // no owner TID latched yet; ran inline
		Overflow,    // queue rejected it; ran inline
		Disabled,    // queue switched off by env; ran inline
		Abandoned,   // teardown in progress; not run
	};

	const char* modeName(Mode mode);

	// Parse a bounded millisecond setting. Pure so the clamping policy is
	// testable: unset/empty keeps `fallback`, a value above `maxMs` or any
	// trailing garbage is refused (also keeping `fallback`), and 0 is a
	// legitimate value with a documented meaning.
	inline unsigned int parseBoundedMs(const char* value, unsigned int fallback,
	                                   unsigned int maxMs, bool* rejected = nullptr)
	{
		if (rejected != nullptr)
			*rejected = false;
		if (value == nullptr || value[0] == '\0')
			return fallback;

		unsigned long parsed = 0;
		for (const char* p = value; *p != '\0'; ++p)
		{
			if (*p < '0' || *p > '9' || parsed > maxMs)
			{
				if (rejected != nullptr)
					*rejected = true;
				return fallback;
			}
			parsed = parsed * 10 + static_cast<unsigned long>(*p - '0');
		}
		if (parsed > maxMs)
		{
			if (rejected != nullptr)
				*rejected = true;
			return fallback;
		}
		return static_cast<unsigned int>(parsed);
	}

	// Parse the queue master switch. The queue is ON unless explicitly turned
	// off, and only a recognised falsy word turns it off — an unrecognised
	// value must not silently disable hardening, so it keeps the default and
	// is reported as invalid.
	inline bool parseQueueEnabled(const char* value, bool fallback = true,
	                              bool* rejected = nullptr)
	{
		if (rejected != nullptr)
			*rejected = false;
		if (value == nullptr || value[0] == '\0')
			return fallback;

		std::string v(value);
		for (char& c : v)
			c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);

		if (v == "0" || v == "off" || v == "no" || v == "false" || v == "disable"
		    || v == "disabled" || v == "n")
			return false;
		if (v == "1" || v == "on" || v == "yes" || v == "true" || v == "enable"
		    || v == "enabled" || v == "y")
			return true;

		if (rejected != nullptr)
			*rejected = true;
		return fallback;
	}

	// Read the environment overrides and log the effective policy. Safe to
	// call from the LD_AUDIT preinit path: it starts no thread and touches no
	// Steam memory.
	void init();

	// Latch the owner IPC thread. Called from the hooked
	// IClientUtils::RunIPCFrame — the dispatcher whose thread the VM run
	// measured — and only the first call has an effect.
	void latchOwnerThread();

	bool ownerLatched();
	bool onOwnerThread();

	inline bool compatExecutionAllowed(bool onOwner) noexcept
	{
		return onOwner;
	}

	// Drain point. Cheap enough to call from every hooked RunIPCFrame: it
	// returns immediately unless this thread is the latched owner AND work is
	// pending. Never re-enters itself.
	void drainOnOwnerFrame();

	// Config-watcher hot-add: append the app ids to package 0 and broadcast
	// the license update, in that order, on the owner IPC thread. Returns
	// without waiting for the owner.
	Mode submitHotAdd(const std::vector<std::uint32_t>& appIds);

	// Managed-state watcher handoff. The complete snapshot is coalesced by
	// generation in the owner queue; the older hot-add API remains available
	// until the watcher switches over in a later task.
	Mode submitManagedState(const PackageSnapshot& snapshot);

	// Apply and confirm a per-app compatibility mapping on the owner IPC
	// thread. Package ownership is published only after confirmation.
	Mode submitCompatReadiness(
		std::uint32_t appId,
		std::uint64_t managedGeneration);

	// API-watcher install request. Also non-blocking.
	Mode submitInstallApp(std::uint32_t appId, std::uint32_t library);

	// Staleness sweep. Called from the watcher loop's bounded poll timeout (and
	// from each submit); a no-op unless something has been pending longer than
	// the configured threshold. Never blocks on the owner.
	void idleTick();

	// Called at the START of the hook-placement pass. Everything before it is
	// pre-hook setup, where a teardown is the benign load()-retry no-op rather
	// than a real shutdown.
	void notePlacement();

	// Teardown: refuse further work and abandon anything pending, but ONLY if
	// a placement pass was entered. Returns true when it actually closed the
	// queue. See OwnerQueue::PlacementGate for why this gate exists.
	bool shutdownIfPlaced();

	// Current pending depth (diagnostics/tests).
	std::size_t pendingDepth();
}
