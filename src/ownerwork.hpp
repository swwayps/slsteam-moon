// SPDX-License-Identifier: AGPL-3.0-only
//
// Runs Steam-owned work (package-0 sync, compatibility mapping) on the owner IPC
// thread, the thread Steam dispatches IClientUtils calls on.
//
//  * Fire-and-forget: a submit from another thread enqueues and returns; the
//    owner drains on its next frame (an idle client runs one every few seconds).
//  * Stale escape: work pending longer than kDefaultMaxStaleMs (30 s) runs inline
//    in FIFO order with a warning, so a stalled owner cannot lose a hot-add.
//  * Inline: the owner runs its own submits; other work runs inline when no owner
//    is latched or the queue is full. A snapshot is never refused for capacity:
//    it coalesces into one record, and inline it would run under HotReload's lock.
//  * EnsureCompat runs only on the owner thread; any other runner re-queues it.
//  * Teardown abandons pending work and refuses new work.
#pragma once

#include <cstddef>
#include <cstdint>

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

	// Read the environment overrides and log the effective policy. Safe to
	// call from the LD_AUDIT preinit path: it starts no thread and touches no
	// Steam memory.
	void init();

	// Latch the owner IPC thread. Called from ProcessIPCFrame on IClientUtils
	// calls — the dispatcher whose thread the VM run measured — and only the
	// first call has an effect.
	void latchOwnerThread();

	bool ownerLatched();
	bool onOwnerThread();

	inline bool compatExecutionAllowed(bool onOwner) noexcept
	{
		return onOwner;
	}

	// Generation zero is the valid first managed epoch: the publication map
	// starts there and increments only when an app is forgotten. The app id is
	// the only sentinel in a compatibility-readiness request; the generation is
	// still carried unchanged so publishPreparedBase can reject stale work.
	inline constexpr bool compatReadinessRequestValid(
		std::uint32_t appId,
		std::uint64_t /*managedGeneration*/) noexcept
	{
		return appId != 0;
	}

	// Drain point. Cheap enough to call on every RunInterface frame: it
	// returns immediately unless this thread is the latched owner AND work is
	// pending. Never re-enters itself.
	void drainOnOwnerFrame();

	// Managed-state handoff: reconcile package 0 to the complete snapshot on
	// the owner IPC thread. Snapshots coalesce by generation in the owner
	// queue. Returns without waiting for the owner.
	Mode submitManagedState(const PackageSnapshot& snapshot);

	// Apply and confirm a per-app compatibility mapping on the owner IPC
	// thread. Package ownership is published only after confirmation.
	Mode submitCompatReadiness(
		std::uint32_t appId,
		std::uint64_t managedGeneration);

	// Staleness sweep. Called from the watcher loop's bounded poll timeout (and
	// from each submit); a no-op unless something has been pending longer than
	// the configured threshold. Never blocks on the owner.
	void idleTick();

	// Called at the START of the hook-placement pass; a teardown before it does
	// not close the queue.
	void notePlacement();

	// Teardown: refuse further work and abandon anything pending, but ONLY if
	// a placement pass was entered. Returns true when it actually closed the
	// queue. See OwnerQueue::PlacementGate for why this gate exists.
	bool shutdownIfPlaced();

}
