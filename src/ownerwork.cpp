// SPDX-License-Identifier: AGPL-3.0-only
//
// Owner-IPC-thread execution site for watcher-originated Steam-owned work.
// See ownerwork.hpp for scope, the fire-and-forget rationale, and the
// staleness escape hatch.

#include "ownerwork.hpp"

#include "afftrace.hpp"
#include "log.hpp"
#include "utils/ownerqueue.hpp"

#include "feats/appinfostate.hpp"
#include "feats/compatlive.hpp"
#include "feats/hotreload.hpp"
#include "feats/packagepatch.hpp"
#include "sdk/IClientAppManager.hpp"
#include "sdk/CSteamEngine.hpp"

#include <atomic>
#include <cstdlib>

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

	// Tracks whether the hook-placement pass was entered, so a teardown from
	// the benign pre-hook load()-retry path does not close the queue for the
	// rest of the session.
	OwnerQueue::PlacementGate& placementGate()
	{
		static OwnerQueue::PlacementGate gate;
		return gate;
	}

	std::atomic<long>         g_ownerTid{ 0 };
	std::atomic<unsigned int> g_maxStaleMs{ OwnerWork::kDefaultMaxStaleMs };
	std::atomic<bool>         g_queueEnabled{ true };

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

	AffTrace::Src traceSrc(OwnerQueue::Kind kind)
	{
		return (kind == OwnerQueue::Kind::InstallApp)
			? AffTrace::Src::Api : AffTrace::Src::Config;
	}

	AffTrace::Call traceCall(OwnerQueue::Kind kind)
	{
		switch (kind)
		{
			case OwnerQueue::Kind::InjectPackage0:    return AffTrace::Call::Package0Inject;
			case OwnerQueue::Kind::ReconcileLicenses: return AffTrace::Call::LicenseReconcile;
			case OwnerQueue::Kind::InstallApp:        return AffTrace::Call::InstallApp;
			case OwnerQueue::Kind::SyncPackage0:     return AffTrace::Call::Package0Sync;
			case OwnerQueue::Kind::EnsureCompat:     return AffTrace::Call::CompatMapping;
		}
		return AffTrace::Call::None;
	}

	// Execution-site label used in the trace. The submit-time Mode and the
	// drain/flush sites are distinct, so this takes the trace mode directly.
	AffTrace::Mode traceMode(OwnerWork::Mode mode)
	{
		switch (mode)
		{
			case OwnerWork::Mode::Queued:    return AffTrace::Mode::Queued;
			case OwnerWork::Mode::OnOwner:   return AffTrace::Mode::Direct;
			case OwnerWork::Mode::NoOwner:   return AffTrace::Mode::DirectNoOwner;
			case OwnerWork::Mode::Overflow:  return AffTrace::Mode::DirectOverflow;
			case OwnerWork::Mode::Disabled:  return AffTrace::Mode::DirectDisabled;
			case OwnerWork::Mode::Abandoned: return AffTrace::Mode::NA;
		}
		return AffTrace::Mode::NA;
	}

	// Execute one command. This is the ONLY place the Steam-owned calls are
	// reached from, whichever thread ends up running them, so the call set,
	// inputs and order are identical in every mode.
	void runCommand(const OwnerQueue::Command& cmd, AffTrace::Mode mode)
	{
		auto span = AffTrace::callSpan(traceSrc(cmd.kind()), traceCall(cmd.kind()), mode);
		switch (cmd.kind())
		{
			case OwnerQueue::Kind::InjectPackage0:
				PackagePatch::injectIntoPackage0(cmd.appIds());
				break;
			case OwnerQueue::Kind::ReconcileLicenses:
				PackagePatch::forceReconcileLicenses();
				break;
			case OwnerQueue::Kind::SyncPackage0:
				PackagePatch::synchronizePackage0(cmd.packageSnapshot());
				break;
			case OwnerQueue::Kind::InstallApp:
				if (g_pClientAppManager == nullptr)
				{
					g_pLog->info("OwnerWork: app manager not available; install request dropped\n");
					break;
				}
				g_pClientAppManager->installApp(cmd.appId(), cmd.library());
				break;
			case OwnerQueue::Kind::EnsureCompat:
			{
				// This internal ConfigStore mutation is owner-only. If a generic
				// fallback reaches it, put it back unchanged for a later IPC frame.
				if (!OwnerWork::compatExecutionAllowed(
					OwnerWork::onOwnerThread()))
				{
					(void)queue().push(
						OwnerQueue::Command::ensureCompat(
							cmd.appId(), cmd.managedGeneration(), cmd.attempt()),
						OwnerQueue::monotonicUs());
					break;
				}

				const auto result = CompatLive::step(
					getLocalClientCompat(), cmd.appId(), cmd.attempt() != 0);
				if (result.status == CompatLive::StepStatus::Ready)
				{
					(void)HotReload::publishPreparedBase(
						cmd.appId(), cmd.managedGeneration());
					break;
				}

				const std::uint32_t nextAttempt = cmd.attempt() + 1;
				if (nextAttempt >= CompatLive::kMaxPollAttempts)
				{
					g_pLog->warn(
						"OwnerWork: live compatibility mapping did not settle for app=%u; "
						"keeping it hidden until restart\n",
						cmd.appId());
					break;
				}
				(void)queue().push(
					OwnerQueue::Command::ensureCompat(
						cmd.appId(), cmd.managedGeneration(), nextAttempt),
					OwnerQueue::monotonicUs());
				break;
			}
		}
	}

	// Runner for a given execution site. Exceptions are contained by the queue
	// itself (per command), so a throwing Steam call can never escape into an
	// IPC frame or a watcher callback.
	OwnerQueue::Queue::Runner runner(AffTrace::Mode mode)
	{
		return [mode](const OwnerQueue::Command& cmd) { runCommand(cmd, mode); };
	}

	// Mandatory inline fallback. Runs anything already pending FIRST, then this
	// batch, inside one exclusive execution window, so the total order of
	// Steam-owned calls is preserved even here.
	void runInline(const std::vector<OwnerQueue::Command>& batch, OwnerWork::Mode mode)
	{
		const std::size_t ran = queue().runPendingThen(batch, runner(traceMode(mode)));
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

		// Queue switched off: keep everything on the calling thread, which is
		// exactly the pre-hardening behaviour.
		if (!g_queueEnabled.load(std::memory_order_relaxed))
		{
			runInline(batch, OwnerWork::Mode::Disabled);
			return OwnerWork::Mode::Disabled;
		}

		// Flush an overdue backlog before adding to it, so the escape hatch
		// cannot be starved by a stream of new events and so ordering stays
		// oldest-first.
		OwnerWork::idleTick();

		// No owner thread known yet (very early boot) — behave exactly as
		// before rather than sit on work nobody will drain.
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

		// Fire and forget: the owner drains on its next frame. Nothing below
		// waits, so the watcher callback returns in microseconds.
		const std::size_t depth = queue().depth();
		for (const OwnerQueue::Command& cmd : batch)
		{
			AffTrace::queuePush(traceSrc(cmd.kind()), traceCall(cmd.kind()),
			                    AffTrace::Mode::Queued,
			                    static_cast<std::uint32_t>(depth));
		}
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
			case Mode::Disabled:  return "direct-queue-disabled";
			case Mode::Abandoned: return "not-run-shutdown";
		}
		return "unknown";
	}

	void init()
	{
		bool rejected = false;
		const bool on = parseQueueEnabled(std::getenv("SLSSTEAM_OWNER_QUEUE"), true,
		                                  &rejected);
		if (rejected)
		{
			g_pLog->warn("OwnerWork: ignoring unrecognised SLSSTEAM_OWNER_QUEUE "
			             "(expected on/off); queue stays enabled\n");
		}
		g_queueEnabled.store(on, std::memory_order_relaxed);

		rejected = false;
		const unsigned int stale =
			parseBoundedMs(std::getenv("SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS"),
			               kDefaultMaxStaleMs, kMaxMaxStaleMs, &rejected);
		if (rejected)
		{
			g_pLog->warn("OwnerWork: ignoring invalid SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS "
			             "(expected 0-%u)\n", kMaxMaxStaleMs);
		}
		g_maxStaleMs.store(stale, std::memory_order_relaxed);

		// The blocking handoff this replaced had its own knob. Measurements
		// showed the wait always expired, so it is gone; say so instead of
		// silently ignoring a setting someone may still have in a wrapper.
		if (std::getenv("SLSSTEAM_OWNER_QUEUE_DEADLINE_MS") != nullptr)
		{
			g_pLog->warn("OwnerWork: SLSSTEAM_OWNER_QUEUE_DEADLINE_MS is obsolete and "
			             "ignored (the handoff no longer blocks); use "
			             "SLSSTEAM_OWNER_QUEUE=off or SLSSTEAM_OWNER_QUEUE_MAX_STALE_MS\n");
		}

		if (!on)
		{
			g_pLog->info("OwnerWork: owner-thread handoff disabled; watcher-originated "
			             "Steam calls run on the calling thread\n");
			return;
		}
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
		AffTrace::latchOwner();
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
		if (depthHint == 0 && !resolvedHint && !refreshPending)
			return;
		if (!OwnerQueue::shouldDrain(cachedTid(),
		                            g_ownerTid.load(std::memory_order_relaxed),
		                            t_draining, depthHint == 0 ? 1 : depthHint))
			return;

		t_draining = true;
		const std::size_t taken = depthHint == 0
			? 0 : queue().drain(runner(AffTrace::Mode::Queued));
		if (AppInfoState::takeResolvedDirty() ||
			PackagePatch::runtimeRefreshPending())
			PackagePatch::reprocessCurrentState();
		t_draining = false;

		if (taken != 0)
		{
			AffTrace::queueDrain(taken, static_cast<std::uint32_t>(queue().depth()));
			g_pLog->info("OwnerWork: drained %zu command(s) on the owner IPC thread\n", taken);
		}
	}

	void idleTick()
	{
		if (!g_queueEnabled.load(std::memory_order_relaxed))
			return;
		const unsigned int staleMs = g_maxStaleMs.load(std::memory_order_relaxed);
		if (staleMs == 0)
			return;  // explicitly configured to accept eventual execution
		// Cheap: one relaxed load when there is nothing pending, which is the
		// normal case for both watcher threads.
		if (queue().depthHint() == 0)
			return;

		const std::uint64_t now = OwnerQueue::monotonicUs();
		const std::size_t flushed = queue().flushStale(
			now, static_cast<std::uint64_t>(staleMs) * 1000ULL,
			runner(AffTrace::Mode::DirectStale));
		if (flushed == 0)
			return;

		AffTrace::queueStale(flushed, static_cast<std::uint32_t>(queue().depth()));
		g_pLog->warn("OwnerWork: owner IPC thread did not drain within %ums; ran %zu "
		             "command(s) on this thread instead\n", staleMs, flushed);
	}

	Mode submitHotAdd(const std::vector<std::uint32_t>& appIds)
	{
		if (appIds.empty())
		{
			// Preserve the existing order: injection first, then the license
			// broadcast. With no ids there is nothing to inject.
			return submitBatch({ OwnerQueue::Command::reconcileLicenses() });
		}
		return submitBatch({ OwnerQueue::Command::injectPackage0(appIds),
		                     OwnerQueue::Command::reconcileLicenses() });
	}

	Mode submitManagedState(const PackageSnapshot& snapshot)
	{
		return submitBatch({ OwnerQueue::Command::syncPackage0(snapshot) });
	}

	Mode submitCompatReadiness(
		std::uint32_t appId,
		std::uint64_t managedGeneration)
	{
		if (appId == 0 || managedGeneration == 0)
			return Mode::Abandoned;
		return submitBatch({ OwnerQueue::Command::ensureCompat(
			appId, managedGeneration, 0) });
	}

	Mode submitInstallApp(std::uint32_t appId, std::uint32_t library)
	{
		return submitBatch({ OwnerQueue::Command::installApp(appId, library) });
	}

	void notePlacement()
	{
		placementGate().notePlacement();
	}

	bool shutdownIfPlaced()
	{
		return OwnerQueue::applyTeardown(placementGate(), queue());
	}

	std::size_t pendingDepth()
	{
		return queue().depth();
	}
}
