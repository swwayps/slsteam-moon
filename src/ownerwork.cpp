// SPDX-License-Identifier: AGPL-3.0-only
//
// Owner-IPC-thread execution site for Steam-owned work.
// See ownerwork.hpp for the dispatch invariants.

#include "ownerwork.hpp"

#include "log.hpp"
#include "utils/ownerqueue.hpp"

#include "feats/appinfostate.hpp"
#include "feats/compatlive.hpp"
#include "feats/hotreload.hpp"
#include "feats/packagepatch.hpp"
#include "sdk/CSteamEngine.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

namespace
{
	// Function-local statics: constructed on first use, so nothing depends on
	// static-init order inside the LD_AUDIT preinit path.
	OwnerQueue::Queue& queue()
	{
		static OwnerQueue::Queue instance;
		return instance;
	}

	// Tracks whether the hook-placement pass was entered, so a teardown before
	// any hook went in does not close the queue for the rest of the session.
	OwnerQueue::PlacementGate& placementGate()
	{
		static OwnerQueue::PlacementGate gate;
		return gate;
	}

	std::atomic<long>         g_ownerTid{ 0 };
	std::atomic<unsigned int> g_maxStaleMs{ OwnerWork::kDefaultMaxStaleMs };

	// Cached native thread id. The drain point sits on Steam's IPC hot path,
	// so it must not pay a gettid syscall per frame.
	long cachedTid()
	{
		static thread_local long tid = 0;
		if (tid == 0)
			tid = static_cast<long>(syscall(SYS_gettid));
		return tid;
	}

	// Guards against a drain reached from inside a drain (a nested owner
	// frame). The queue refuses re-entrant claims too; this keeps us from
	// even reaching it.
	thread_local bool t_draining = false;

	// Queue an EnsureCompat again for a later owner frame. Every retry counts;
	// past kMaxPollAttempts, or when the queue refuses it, the app stays hidden
	// until the restart applies the persisted mapping. A held lock is not a
	// failed attempt: parkBusy bounds that wait by time instead.
	void retryCompat(const OwnerQueue::Command& cmd, const char* reason)
	{
		const std::uint32_t next = cmd.attempt() + 1;
		if (next < CompatLive::kMaxPollAttempts)
		{
			const auto pushed = queue().push(
				OwnerQueue::Command::ensureCompat(
					cmd.appId(), cmd.managedGeneration(), next, cmd.busySinceUs()),
				OwnerQueue::monotonicUs());
			if (pushed != OwnerQueue::PushResult::Full)
				return;
			reason = "owner queue full";
		}
		g_pLog->warn("OwnerWork: %s for app=%u; keeping it hidden until restart\n",
		             reason, cmd.appId());
	}

	// A publication refused by a held lock waits here rather than in the
	// queue, so owner frames that come faster than the lock is released do
	// not each retry it. Owner thread only: parkBusy runs behind the compat
	// gate and releaseBusy inside the owner drain.
	constexpr std::uint64_t kBusyRetryUs = 250000;
	constexpr std::uint64_t kBusyLimitUs = 60000000;
	std::vector<OwnerQueue::Command> g_busy;
	std::atomic<std::uint64_t> g_busyDueUs{ 0 };  // 0: nothing parked

	void parkBusy(const OwnerQueue::Command& cmd)
	{
		const std::uint64_t now = OwnerQueue::monotonicUs();
		const std::uint64_t since = cmd.busySinceUs() != 0 ? cmd.busySinceUs() : now;
		if (now - since >= kBusyLimitUs)
		{
			g_pLog->warn("OwnerWork: hot reload stayed busy for app=%u; keeping it "
			             "hidden until restart\n", cmd.appId());
			return;
		}
		g_busy.push_back(OwnerQueue::Command::ensureCompat(
			cmd.appId(), cmd.managedGeneration(), cmd.attempt(), since));
		if (g_busyDueUs.load(std::memory_order_relaxed) == 0)
			g_busyDueUs.store(now + kBusyRetryUs, std::memory_order_relaxed);
	}

	bool busyRetryDue()
	{
		const std::uint64_t due = g_busyDueUs.load(std::memory_order_relaxed);
		return due != 0 && OwnerQueue::monotonicUs() >= due;
	}

	void releaseBusy()
	{
		g_busyDueUs.store(0, std::memory_order_relaxed);
		std::vector<OwnerQueue::Command> parked;
		parked.swap(g_busy);
		for (const OwnerQueue::Command& cmd : parked)
		{
			if (queue().push(cmd, OwnerQueue::monotonicUs()) == OwnerQueue::PushResult::Full)
				g_pLog->warn("OwnerWork: owner queue full for app=%u; keeping it "
				             "hidden until restart\n", cmd.appId());
		}
	}

	// Steam's dispatcher must not wait on a pass or publication, so a held
	// lock parks the request for a later frame.
	void publishOrPark(const OwnerQueue::Command& cmd)
	{
		bool busy = false;
		(void)HotReload::publishPreparedBase(
			cmd.appId(), cmd.managedGeneration(), &busy);
		if (busy)
			parkBusy(cmd);
	}

	// Execute one command. Every dispatch mode reaches the Steam-owned calls
	// only through here.
	void runCommand(const OwnerQueue::Command& cmd)
	{
		switch (cmd.kind())
		{
			case OwnerQueue::Kind::SyncPackage0:
				PackagePatch::synchronizePackage0(cmd.packageSnapshot());
				break;
			case OwnerQueue::Kind::EnsureCompat:
			{
				// This internal ConfigStore mutation is owner-only. If a generic
				// fallback reaches it, put it back unchanged for a later IPC frame.
				if (!OwnerWork::compatExecutionAllowed(
					OwnerWork::onOwnerThread()))
				{
					retryCompat(cmd, "owner IPC thread never ran the compatibility mapping");
					break;
				}

				// A busy retry issued or found its mapping on its first run;
				// only the publication is left.
				if (cmd.busySinceUs() != 0)
				{
					publishOrPark(cmd);
					break;
				}

				IClientCompat* const compat = g_pClientCompat;
				if (compat == nullptr)
				{
					// No live compatibility manager to talk to yet. Retry a
					// bounded number of frames (the CCompatManager detour
					// captures it on Steam's first compat call, within the first
					// frames after boot); if it never appears, the mapping we already
					// persisted for the next preinit pass applies on restart, so
					// keep the title hidden until then rather than publish a
					// windows-only app Steam would treat as "no applicable
					// platform" (0 mounted depots).
					retryCompat(cmd, "compat manager unavailable");
					break;
				}

				const auto result = CompatLive::step(compat, cmd.appId());

				// Ready       -> an effective mapping already resolves for the
				//                app (a config.vdf entry loaded at boot, a global
				//                default, or a live specify that reflected at
				//                once). Publish it into ownership now.
				// Requested   -> specifyCompatTool has been issued. Steam stores
				//                and persists that mapping synchronously (it
				//                reappears in config.vdf), but getCompatToolName
				//                only reflects it after an internal refresh that
				//                can lag several seconds. That readback is not a
				//                precondition for the title to install or launch
				//                through Proton, so publish rather than withhold
				//                ownership until a restart the user never asked
				//                for. This is the difference between the title
				//                appearing in ~1-2s and it staying at "0 B"
				//                until the next launch.
				if (result.status != CompatLive::StepStatus::Unavailable)
				{
					publishOrPark(cmd);
					break;
				}

				// Unavailable: a call into the compat interface faulted. Retry a
				// bounded number of times before deferring to a restart.
				retryCompat(cmd, "live compatibility mapping did not settle");
				break;
			}
		}
	}

	// Mandatory inline fallback. Runs anything already pending FIRST, then this
	// batch, inside one exclusive execution window, so the total order of
	// Steam-owned calls is preserved even here. The queue contains exceptions
	// per command, so a throwing Steam call never escapes into the caller.
	void runInline(const std::vector<OwnerQueue::Command>& batch, OwnerWork::Mode mode)
	{
		const std::size_t ran = queue().runPendingThen(batch, runCommand);
		if (ran > batch.size())
		{
			g_pLog->info("OwnerWork: ran %zu queued command(s) inline alongside the "
			             "%s fallback\n", ran - batch.size(), OwnerWork::modeName(mode));
		}
	}

	OwnerWork::Mode submitBatch(const std::vector<OwnerQueue::Command>& batch)
	{
		if (batch.empty())
			return OwnerWork::Mode::Abandoned;

		// Teardown: never start new Steam-owned work.
		if (queue().isShuttingDown())
		{
			g_pLog->debug("OwnerWork: shutting down; %zu command(s) not run\n", batch.size());
			return OwnerWork::Mode::Abandoned;
		}

		// Flush an overdue backlog before adding to it, so the escape hatch
		// cannot be starved by a stream of new events and so ordering stays
		// oldest-first.
		OwnerWork::idleTick();

		// No owner latched yet (early boot): run inline rather than queue work
		// nobody drains.
		if (!OwnerWork::ownerLatched())
		{
			runInline(batch, OwnerWork::Mode::NoOwner);
			return OwnerWork::Mode::NoOwner;
		}

		// Already the owner thread: run inline, that IS the target thread.
		if (OwnerWork::onOwnerThread())
		{
			runInline(batch, OwnerWork::Mode::OnOwner);
			return OwnerWork::Mode::OnOwner;
		}

		if (!queue().pushBatch(batch, OwnerQueue::monotonicUs()))
		{
			runInline(batch, OwnerWork::Mode::Overflow);
			return OwnerWork::Mode::Overflow;
		}

		// Fire and forget: the owner drains on its next frame.
		return OwnerWork::Mode::Queued;
	}
}

namespace OwnerWork
{
	const char* modeName(Mode mode)
	{
		switch (mode)
		{
			case Mode::Queued:    return "queued-owner-thread";
			case Mode::OnOwner:   return "owner-thread-inline";
			case Mode::NoOwner:   return "direct-no-owner";
			case Mode::Overflow:  return "direct-queue-full";
			case Mode::Abandoned: return "not-run-shutdown";
		}
		return "unknown";
	}

	void init()
	{
		// Setting SLSSTEAM_OWNER_QUEUE only logs a warning; it cannot turn the
		// queue off, because inline runs on the submitting thread can deadlock
		// the owner IPC thread.
		if (std::getenv("SLSSTEAM_OWNER_QUEUE") != nullptr)
		{
			g_pLog->warn("OwnerWork: SLSSTEAM_OWNER_QUEUE is no longer supported and "
			             "is ignored\n");
		}

		bool rejected = false;
		const unsigned int stale =
			parseBoundedMs(std::getenv("SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS"),
			               kDefaultMaxStaleMs, kMaxMaxStaleMs, &rejected);
		if (rejected)
		{
			g_pLog->warn("OwnerWork: ignoring invalid SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS "
			             "(expected 0-%u)\n", kMaxMaxStaleMs);
		}
		g_maxStaleMs.store(stale, std::memory_order_relaxed);

		g_pLog->info("OwnerWork: watcher-originated Steam calls hand off to the owner "
		             "IPC thread (non-blocking; stale fallback %s)\n",
		             stale == 0 ? "off" : "on");
		if (stale != 0)
			g_pLog->debug("OwnerWork: stale fallback after %ums\n", stale);
	}

	void latchOwnerThread()
	{
		if (g_ownerTid.load(std::memory_order_relaxed) != 0)
			return;
		long expected = 0;
		const long self = cachedTid();
		if (!g_ownerTid.compare_exchange_strong(expected, self))
			return;
		g_pLog->debug("OwnerWork: latched owner IPC thread\n");
	}

	bool ownerLatched()
	{
		return g_ownerTid.load(std::memory_order_relaxed) != 0;
	}

	bool onOwnerThread()
	{
		const long owner = g_ownerTid.load(std::memory_order_relaxed);
		return owner != 0 && owner == cachedTid();
	}

	void drainOnOwnerFrame()
	{
		// Two lock-free hints in the overwhelmingly common idle case: queued
		// commands and appinfo metadata completion share this owner boundary.
		const std::size_t depthHint = queue().depthHint();
		const bool resolvedHint = AppInfoState::resolvedDirtyHint();
		const bool refreshPending = PackagePatch::runtimeRefreshPending();
		const bool busyDue = busyRetryDue();
		if (depthHint == 0 && !resolvedHint && !refreshPending && !busyDue)
			return;
		if (!OwnerQueue::shouldDrain(cachedTid(),
		                            g_ownerTid.load(std::memory_order_relaxed),
		                            t_draining,
		                            depthHint != 0 || resolvedHint || refreshPending || busyDue))
			return;

		t_draining = true;
		if (busyDue)
			releaseBusy();
		const std::size_t taken = queue().depthHint() == 0
			? 0 : queue().drain(runCommand);
		if (AppInfoState::takeResolvedDirty() ||
			PackagePatch::runtimeRefreshPending())
			PackagePatch::reprocessCurrentState();
		t_draining = false;

		if (taken != 0)
			g_pLog->info("OwnerWork: drained %zu command(s) on the owner IPC thread\n", taken);
	}

	void idleTick()
	{
		const unsigned int staleMs = g_maxStaleMs.load(std::memory_order_relaxed);
		if (staleMs == 0)
			return;  // explicitly configured to accept eventual execution
		// Cheap: one relaxed load when there is nothing pending, which is the
		// normal case for both watcher threads.
		if (queue().depthHint() == 0)
			return;

		const std::uint64_t now = OwnerQueue::monotonicUs();
		const std::size_t flushed = queue().flushStale(
			now, static_cast<std::uint64_t>(staleMs) * 1000ULL, runCommand);
		if (flushed == 0)
			return;

		g_pLog->warn("OwnerWork: owner IPC thread did not drain within %ums; ran %zu "
		             "command(s) on this thread instead\n", staleMs, flushed);
	}

	Mode submitManagedState(const PackageSnapshot& snapshot)
	{
		return submitBatch({ OwnerQueue::Command::syncPackage0(snapshot) });
	}

	Mode submitCompatReadiness(
		std::uint32_t appId,
		std::uint64_t managedGeneration)
	{
		if (!compatReadinessRequestValid(appId, managedGeneration))
			return Mode::Abandoned;
		return submitBatch({ OwnerQueue::Command::ensureCompat(
			appId, managedGeneration, 0) });
	}


	void notePlacement()
	{
		placementGate().notePlacement();
	}

	bool shutdownIfPlaced()
	{
		return OwnerQueue::applyTeardown(placementGate(), queue());
	}

}
