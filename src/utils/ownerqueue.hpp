// SPDX-License-Identifier: AGPL-3.0-only
//
// Bounded owner-thread command queue (thread-affinity HARDENING, not a fix).
//
// WHY THIS EXISTS
// ---------------
// The config watcher (and the API watcher) are plain inotify pthreads. Today
// they reach into Steam-owned code directly: the package-0 hot-add path calls
// the resolved `CUtlMemoryGrow` on a live CUtlVector, and the license
// reconcile calls Steam's `NotifyLicensesUpdated`. A runtime trace on a
// controlled VM confirmed this: those Steam-owned calls executed on the
// watcher pthread while the latched owner thread of
// `IClientUtils::RunIPCFrame` was a different thread.
//
// That is an unwanted execution site regardless of whether it ever causes a
// user-visible failure: it is off-owner mutation of Steam-owned state. This
// queue exists so watcher-originated Steam-owned work can be handed to the
// owner IPC thread instead. It is DEFENSIVE HARDENING plus diagnostics. It is
// NOT presented as the cause of, or the fix for, any cumulative client
// failure — that investigation is inconclusive and no causal claim is made
// here.
//
// FIRE-AND-FORGET HANDOFF (measured, not guessed)
// -----------------------------------------------
// The first version of this queue made the watcher thread WAIT for the owner
// to pick the work up, with a 1500 ms deadline and a direct fallback. Guest
// measurements killed that design:
//
//   * owner-thread wake cadence on an idle client: ~3.30 s and ~3.41 s, twice
//     independently;
//   * adding five extra drain points (one per hooked dispatcher) did NOT
//     shorten it (3.30 s -> 3.41 s), so drain DENSITY is not the limiter — the
//     client simply does not run IPC frames more often when idle;
//   * so at 1500 ms the wait ALWAYS expired: the watcher paid 1.5 s and then
//     ran the work off-owner anyway. Worst of both.
//
// The handoff is therefore fire-and-forget: `push`/`pushBatch` return
// immediately and the owner drains whenever it next runs. The hot-add is
// already asynchronous from the user's point of view (a .lua drops into
// stplug-in and the library updates a moment later), so a few seconds of
// handoff latency costs nothing, while the watcher callback goes back to
// milliseconds.
//
// The one case fire-and-forget must still answer is "what if the owner never
// drains at all?" (client wedged, dispatcher thread replaced, IPC stopped).
// Accepting eventual execution would mean a hot-add could silently never
// apply, which is worse than today. So there is an explicit BOUNDED STALENESS
// escape hatch: `flushStale()` runs the whole pending batch inline, in order,
// once the oldest entry is older than a threshold chosen far above the
// measured cadence (default 30 s ~= 9x the 3.3-3.4 s observed). See
// ownerwork.hpp for the policy and the caller.
//
// DESIGN CONSTRAINTS
// ------------------
//  * Immutable commands. A stored command is never mutated; coalescing
//    REPLACES a record with a freshly built one and KEEPS the older record's
//    enqueue timestamp, so a stream of coalescing pushes cannot postpone the
//    staleness escape hatch for ever.
//  * Bounded. A fixed pending capacity and a per-command id cap. On overflow
//    the push is REJECTED so the caller can run the work itself — semantics
//    are preserved, only the execution site degrades to today's behaviour.
//  * Coalescing. Two pending package-0 injections merge into one whose id list
//    is the first-seen-order union, which is exactly what running them in
//    sequence would leave behind (injection is per-id idempotent). A second
//    pending license reconcile is redundant, as is a duplicate install of the
//    same (app, library) pair.
//  * Idempotent enqueue. Re-pushing work already pending is a no-op that
//    reports `Duplicate`.
//  * Exactly once. Every accepted command is executed by exactly one of the
//    owner drain, the staleness flush, or an inline fallback — the pending set
//    is always claimed under the lock by swapping it out whole.
//  * In order. Claims are all-or-nothing over the whole pending set, and the
//    inline fallbacks run any pending work BEFORE the batch they were given,
//    so the total order of Steam-owned calls never inverts.
//  * Serialised execution. One recursive execution mutex covers every path, so
//    two threads can never run this subsystem's Steam-owned work at the same
//    time, whichever mode each of them is in.
//  * Shutdown rejection. After `shutdown()` nothing is accepted and pending
//    work is abandoned, so teardown never races new Steam-owned calls in.
//  * Exception containment. A throwing command cannot escape into Steam's IPC
//    frame or a watcher callback, and cannot stop later commands from running.
//  * No threads. This file creates none: the owner thread drains, the
//    enqueuing thread returns immediately. (Spawning threads from the
//    LD_AUDIT preinit path is a recorded anti-pattern in this repo.)
//
// Pure logic + std threading primitives only, so it is host-unit-testable
// with no Steam process (see tools/test_ownerqueue.cpp).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "../feats/hotreload_types.hpp"
#include "../feats/hotreload_package.hpp"

namespace OwnerQueue
{
	// Default pending capacity. The realistic pending set is 1 injection +
	// 1 reconcile + a handful of installs, so 32 is generous; the cap exists
	// to bound memory and to make overflow behaviour explicit and testable.
	inline constexpr std::size_t kDefaultCapacity = 32;

	// Per-command id-list cap. The hot-add command carries the whole active
	// app set, while a managed-state snapshot carries app and depot ids. Each
	// list is checked independently so the bound never relies on overflowing
	// size arithmetic.
	inline constexpr std::size_t kMaxIdsPerCommand = 4096;

	// Monotonic microseconds. Used for enqueue timestamps, so the staleness
	// escape hatch is immune to wall-clock jumps. Passed in explicitly
	// everywhere the queue needs "now", which keeps the staleness decision
	// testable without sleeping.
	inline std::uint64_t monotonicUs()
	{
		struct timespec ts{};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL
		     + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
	}

	enum class Kind : std::uint8_t
	{
		InjectPackage0 = 0,     // CUtlMemoryGrow on package 0's vectors
		ReconcileLicenses = 1,  // NotifyLicensesUpdated broadcast
		InstallApp = 2,         // IClientAppManager::InstallApp
		SyncPackage0 = 3,       // complete managed package-0 snapshot
	};

	// Immutable command record. Construct through the factories; every
	// accessor is const and nothing mutates a stored instance.
	class Command
	{
	public:
		static Command injectPackage0(std::vector<std::uint32_t> appIds)
		{
			return Command(Kind::InjectPackage0, std::move(appIds), 0, 0, {});
		}
		static Command reconcileLicenses()
		{
			return Command(Kind::ReconcileLicenses, {}, 0, 0, {});
		}
		static Command installApp(std::uint32_t appId, std::uint32_t library)
		{
			return Command(Kind::InstallApp, {}, appId, library, {});
		}
		static Command syncPackage0(PackageSnapshot snapshot)
		{
			return Command(Kind::SyncPackage0, {}, 0, 0, std::move(snapshot));
		}

		Kind kind() const { return m_kind; }
		const std::vector<std::uint32_t>& appIds() const { return m_appIds; }
		const PackageSnapshot& packageSnapshot() const { return m_packageSnapshot; }
		std::uint32_t appId() const { return m_appId; }
		std::uint32_t library() const { return m_library; }

		// Idempotency key: two commands describe the same work when they are
		// the same kind and address the same target.
		bool sameWork(const Command& other) const
		{
			if (m_kind != other.m_kind)
				return false;
			switch (m_kind)
			{
				case Kind::InjectPackage0:
					return m_appIds == other.m_appIds;
				case Kind::ReconcileLicenses:
					return true;
				case Kind::InstallApp:
					return m_appId == other.m_appId && m_library == other.m_library;
				case Kind::SyncPackage0:
					return m_packageSnapshot == other.m_packageSnapshot;
			}
			return false;
		}

	private:
		Command(Kind kind, std::vector<std::uint32_t> appIds,
		        std::uint32_t appId, std::uint32_t library,
		        PackageSnapshot packageSnapshot)
			: m_kind(kind), m_appIds(std::move(appIds)),
			  m_packageSnapshot(std::move(packageSnapshot)),
			  m_appId(appId), m_library(library)
		{
		}

		Kind                       m_kind;
		std::vector<std::uint32_t> m_appIds;
		PackageSnapshot            m_packageSnapshot;
		std::uint32_t              m_appId;
		std::uint32_t              m_library;
	};

	enum class PushResult : std::uint8_t
	{
		Queued = 0,        // appended as a new pending command
		Coalesced = 1,     // merged into an existing pending command
		Duplicate = 2,     // already pending, nothing to do (idempotent)
		Full = 3,          // capacity or id cap would be exceeded
		ShuttingDown = 4,  // queue closed; caller must not enqueue
	};

	struct Stats
	{
		std::uint64_t pushed = 0;            // new pending records appended
		std::uint64_t coalesced = 0;         // merged into a pending record
		std::uint64_t duplicates = 0;        // idempotent no-op pushes
		std::uint64_t rejectedFull = 0;      // capacity / id-cap rejections
		std::uint64_t rejectedShutdown = 0;  // pushes after shutdown()
		std::uint64_t drained = 0;           // commands taken by the owner
		std::uint64_t flushedStale = 0;      // commands taken by the escape hatch
		std::uint64_t ranInline = 0;         // commands run by a fallback caller
		std::uint64_t executed = 0;          // commands the runner completed
		std::uint64_t failed = 0;            // commands whose runner threw
		std::uint64_t abandoned = 0;         // pending dropped at shutdown
	};

	// The owner-side affinity gate, as a pure decision so it can be tested
	// without a Steam process. A thread may drain only when there is work, it
	// is not already inside a drain, an owner thread has been latched, and it
	// IS that thread. Everything else returns false, which is why the drain
	// point is safe to call from every hooked dispatcher.
	inline bool shouldDrain(long selfTid, long ownerTid, bool alreadyDraining,
	                        std::size_t depthHint)
	{
		return depthHint != 0
		    && !alreadyDraining
		    && ownerTid != 0
		    && selfTid == ownerTid;
	}

	// Staleness predicate for the escape hatch, pure and therefore testable
	// without waiting. `maxStaleUs == 0` means "never escape, accept eventual
	// execution on the owner thread" — a legitimate configuration.
	inline bool isStale(std::uint64_t oldestUs, std::uint64_t nowUs,
	                    std::uint64_t maxStaleUs)
	{
		if (maxStaleUs == 0 || oldestUs == 0 || nowUs <= oldestUs)
			return false;
		return (nowUs - oldestUs) >= maxStaleUs;
	}

	// Placement gate: tells a REAL teardown apart from the benign pre-hook
	// cleanup path.
	//
	// main.cpp's load() runs once per audited module open. Its "the other
	// module isn't mapped yet" retry calls unload() -> Hooks::remove() before
	// anything has been hooked, which used to be a harmless no-op. Once
	// Hooks::remove() also closes this queue, that no-op would refuse every
	// watcher-originated Steam call for the WHOLE SESSION (work abandoned, not
	// merely executed elsewhere): no package-0 injection, no license
	// broadcast, for as long as the client runs.
	//
	// So the queue is closed only when a placement pass has actually been
	// entered. `notePlacement()` is called at the START of that pass, before
	// the first hook goes in, so a teardown interrupting a partial placement
	// still counts as real.
	class PlacementGate
	{
	public:
		void notePlacement() { m_placed.store(true, std::memory_order_release); }
		bool placed() const { return m_placed.load(std::memory_order_acquire); }

		// True exactly once per placement, so a repeated teardown is a no-op.
		bool claimTeardown() { return m_placed.exchange(false, std::memory_order_acq_rel); }

	private:
		std::atomic<bool> m_placed{ false };
	};

	class Queue
	{
	public:
		using Runner = std::function<void(const Command&)>;

		explicit Queue(std::size_t capacity = kDefaultCapacity,
		               std::size_t maxIdsPerCommand = kMaxIdsPerCommand)
			: m_capacity(capacity ? capacity : 1),
			  m_maxIds(maxIdsPerCommand ? maxIdsPerCommand : 1)
		{
		}

		Queue(const Queue&) = delete;
		Queue& operator=(const Queue&) = delete;

		// Enqueue one command, stamped with `nowUs` for the staleness escape
		// hatch. Never blocks on Steam work: see PushResult for the outcomes.
		PushResult push(const Command& cmd, std::uint64_t nowUs)
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			const PushResult r = pushLocked(cmd, nowUs);
			syncHintLocked();
			return r;
		}

		// Lock-free pending-depth hint. The owner-side drain point runs on
		// Steam's IPC hot path, so it must be able to decide "nothing to do"
		// with a single relaxed load instead of taking the mutex.
		std::size_t depthHint() const
		{
			return m_depthHint.load(std::memory_order_relaxed);
		}

		// All-or-nothing enqueue of an ordered batch. Either every command is
		// accepted (queued, coalesced or recognised as a duplicate) or nothing
		// is applied and the caller runs the whole batch itself — which keeps
		// the batch's relative order intact in both cases. Returns immediately;
		// the owner drains later.
		bool pushBatch(const std::vector<Command>& batch, std::uint64_t nowUs,
		               std::vector<PushResult>* perCommand = nullptr)
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			if (m_shuttingDown)
			{
				m_stats.rejectedShutdown += batch.size();
				if (perCommand)
					perCommand->assign(batch.size(), PushResult::ShuttingDown);
				return false;
			}

			// Dry run against a scratch copy so a rejection leaves the live
			// queue untouched.
			std::vector<Entry> scratch = m_pending;
			for (const Command& cmd : batch)
			{
				if (!applyPush(scratch, cmd, nowUs, nullptr))
				{
					m_stats.rejectedFull += batch.size();
					if (perCommand)
						perCommand->assign(batch.size(), PushResult::Full);
					return false;
				}
			}

			if (perCommand)
				perCommand->clear();
			for (const Command& cmd : batch)
			{
				const PushResult r = pushLocked(cmd, nowUs);
				if (perCommand)
					perCommand->push_back(r);
			}
			syncHintLocked();
			return true;
		}

		// Owner-thread drain. Takes the whole pending batch and runs it in
		// FIFO order under the execution mutex, so it can never overlap an
		// inline fallback on another thread. Each command runs inside its own
		// exception barrier, so one failure neither escapes into Steam's IPC
		// frame nor skips the rest. Returns the number of commands taken.
		//
		// Re-entrant calls (a drain reached from inside a drain) take nothing
		// and return 0, so Steam-owned work can never nest.
		std::size_t drain(const Runner& run)
		{
			std::vector<Entry> batch;
			if (!claim(batch, ClaimKind::Drain, 0, 0))
				return 0;
			runClaimed(batch, run);
			return batch.size();
		}

		// Bounded-staleness escape hatch, for the case the owner never drains
		// (client wedged, dispatcher thread gone, IPC stopped). Runs the WHOLE
		// pending set inline, in FIFO order, on the calling thread — i.e. it
		// deliberately degrades to the pre-existing off-owner execution site
		// rather than letting a hot-add be lost.
		//
		// Takes nothing (returns 0) when the queue is empty, closed, already
		// being executed, or the oldest entry is not yet stale. The caller can
		// therefore poll it cheaply from any thread.
		std::size_t flushStale(std::uint64_t nowUs, std::uint64_t maxStaleUs,
		                       const Runner& run)
		{
			std::vector<Entry> batch;
			if (!claim(batch, ClaimKind::Stale, nowUs, maxStaleUs))
				return 0;
			runClaimed(batch, run);
			return batch.size();
		}

		// Mandatory inline fallback: run everything currently pending and then
		// `extra`, all inside ONE exclusive execution window, in that order.
		//
		// Running the pending set first is what keeps the total order of
		// Steam-owned calls correct when the queue could not accept `extra`
		// (capacity) or must not be used at all (no owner latched yet, queue
		// disabled). Returns the number of commands executed.
		std::size_t runPendingThen(const std::vector<Command>& extra, const Runner& run)
		{
			std::vector<Entry> batch;
			// Best-effort claim: if another thread is already executing, its
			// batch is out of the pending set already, so we simply run
			// `extra` after it (the execution mutex orders us).
			const bool claimed = claim(batch, ClaimKind::Inline, 0, 0);
			for (const Command& cmd : extra)
				batch.push_back(Entry{ cmd, 0 });

			if (batch.empty())
			{
				if (claimed)
					releaseExecuting();
				return 0;
			}
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				m_stats.ranInline += batch.size();
			}
			runBatch(batch, run);
			if (claimed)
				releaseExecuting();
			return batch.size();
		}

		// Close the queue: refuse new work, abandon pending work. Called from
		// the teardown path (through applyTeardown) so unload never races a
		// fresh Steam-owned call in.
		void shutdown()
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			if (m_shuttingDown)
				return;
			m_shuttingDown = true;
			m_stats.abandoned += m_pending.size();
			m_pending.clear();
			syncHintLocked();
		}

		bool isShuttingDown() const
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			return m_shuttingDown;
		}

		std::size_t depth() const
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			return m_pending.size();
		}

		// Age of the oldest pending command, 0 when nothing is pending.
		// Diagnostics only.
		std::uint64_t oldestAgeUs(std::uint64_t nowUs) const
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			if (m_pending.empty())
				return 0;
			const std::uint64_t oldest = m_pending.front().enqueuedUs;
			return (nowUs > oldest) ? nowUs - oldest : 0;
		}

		std::size_t capacity() const { return m_capacity; }

		Stats stats() const
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			return m_stats;
		}

	private:
		// A pending command plus the moment it was accepted. The timestamp is
		// only ever used by the staleness escape hatch.
		struct Entry
		{
			Command       cmd;
			std::uint64_t enqueuedUs = 0;
		};

		enum class ClaimKind : std::uint8_t
		{
			Drain = 0,   // owner IPC frame
			Stale = 1,   // bounded-staleness escape hatch
			Inline = 2,  // mandatory fallback, ordering claim only
		};

		// First-seen-order union of two id lists. Injecting {A} then {A,B}
		// leaves the same package-0 vector contents, in the same order, as
		// injecting {A,B} once, because each id is injected at most once.
		static std::vector<std::uint32_t> mergeIds(
			const std::vector<std::uint32_t>& base,
			const std::vector<std::uint32_t>& extra)
		{
			std::vector<std::uint32_t> out = base;
			for (std::uint32_t id : extra)
			{
				bool seen = false;
				for (std::uint32_t have : out)
				{
					if (have == id)
					{
						seen = true;
						break;
					}
				}
				if (!seen)
					out.push_back(id);
			}
			return out;
		}

		// Apply `cmd` to `into`, honouring coalescing, idempotency and both
		// bounds. Returns false when the command cannot be accepted.
		// `result` (optional) reports how it was accepted.
		bool applyPush(std::vector<Entry>& into, const Command& cmd,
		               std::uint64_t nowUs, PushResult* result) const
		{
			if (cmd.appIds().size() > m_maxIds)
				return false;
			if (cmd.kind() == Kind::SyncPackage0 &&
			    (cmd.packageSnapshot().appIds.size() > m_maxIds ||
			     cmd.packageSnapshot().depotIds.size() > m_maxIds ||
			     cmd.packageSnapshot().addedAppIds.size() > m_maxIds ||
			     cmd.packageSnapshot().appInfoRequestIds.size() > m_maxIds))
				return false;

			for (std::size_t i = 0; i < into.size(); ++i)
			{
				Entry& pending = into[i];
				if (pending.cmd.kind() != cmd.kind())
					continue;

				if (pending.cmd.sameWork(cmd))
				{
					if (result)
						*result = PushResult::Duplicate;
					return true;
				}

				if (cmd.kind() == Kind::InjectPackage0)
				{
					auto merged = mergeIds(pending.cmd.appIds(), cmd.appIds());
					if (merged.size() > m_maxIds)
						return false;
					if (merged.size() == pending.cmd.appIds().size())
					{
						// Every new id was already pending.
						if (result)
							*result = PushResult::Duplicate;
						return true;
					}
					// Keep the ORIGINAL enqueue time: a stream of coalescing
					// pushes must not be able to keep the escape hatch away
					// for ever.
					into[i] = Entry{ Command::injectPackage0(std::move(merged)),
					                 pending.enqueuedUs };
					if (result)
						*result = PushResult::Coalesced;
					return true;
				}

				if (cmd.kind() == Kind::SyncPackage0)
				{
					// A stale or equal-generation update is already represented by
					// the pending record. Report it as Duplicate so the caller
					// knows no newer owner work was added. A newer generation
					// replaces only the payload and keeps the original age.
					if (cmd.packageSnapshot().generation <=
					    pending.cmd.packageSnapshot().generation)
					{
						if (result)
							*result = PushResult::Duplicate;
						return true;
					}

					const std::uint64_t oldest = pending.enqueuedUs;
					PackageSnapshot replacement =
						HotReloadPackage::carryPendingSnapshotWork(
							pending.cmd.packageSnapshot(), cmd.packageSnapshot(),
							false, false);
					if (replacement.addedAppIds.size() > m_maxIds ||
						replacement.appInfoRequestIds.size() > m_maxIds)
						return false;
					into[i] = Entry{
						Command::syncPackage0(std::move(replacement)), oldest };
					if (result)
						*result = PushResult::Coalesced;
					return true;
				}
			}

			if (into.size() >= m_capacity)
				return false;

			into.push_back(Entry{ cmd, nowUs });
			if (result)
				*result = PushResult::Queued;
			return true;
		}

		PushResult pushLocked(const Command& cmd, std::uint64_t nowUs)
		{
			if (m_shuttingDown)
			{
				++m_stats.rejectedShutdown;
				return PushResult::ShuttingDown;
			}

			PushResult result = PushResult::Queued;
			if (!applyPush(m_pending, cmd, nowUs, &result))
			{
				++m_stats.rejectedFull;
				return PushResult::Full;
			}

			switch (result)
			{
				case PushResult::Queued:    ++m_stats.pushed; break;
				case PushResult::Coalesced: ++m_stats.coalesced; break;
				case PushResult::Duplicate: ++m_stats.duplicates; break;
				default: break;
			}
			return result;
		}

		// Take the whole pending set, all-or-nothing, and mark the queue as
		// executing so no other path can claim work until this batch is done.
		// Claiming everything is what preserves total order.
		bool claim(std::vector<Entry>& out, ClaimKind kind,
		           std::uint64_t nowUs, std::uint64_t maxStaleUs)
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			if (m_executing)
				return false;
			if (kind == ClaimKind::Stale && m_shuttingDown)
				return false;
			if (m_pending.empty())
			{
				// An Inline claim still needs the executing flag so a
				// concurrent owner frame cannot interleave with the batch the
				// caller is about to run.
				if (kind != ClaimKind::Inline)
					return false;
				m_executing = true;
				return true;
			}
			if (kind == ClaimKind::Stale
			    && !isStale(m_pending.front().enqueuedUs, nowUs, maxStaleUs))
				return false;

			out.swap(m_pending);
			m_executing = true;
			if (kind == ClaimKind::Drain)
				m_stats.drained += out.size();
			else if (kind == ClaimKind::Stale)
				m_stats.flushedStale += out.size();
			syncHintLocked();
			return true;
		}

		// Run a batch under the execution mutex, each command inside its own
		// exception barrier. Touches no claim state.
		void runBatch(const std::vector<Entry>& batch, const Runner& run)
		{
			// Recursive so a nested inline run on the SAME thread (which the
			// callers avoid, but which must never deadlock) is safe.
			std::lock_guard<std::recursive_mutex> ex(m_exec);
			for (const Entry& entry : batch)
			{
				bool threw = false;
				try
				{
					if (run)
						run(entry.cmd);
				}
				catch (...)
				{
					threw = true;
				}
				std::lock_guard<std::mutex> lk(m_mutex);
				if (threw)
					++m_stats.failed;
				else
					++m_stats.executed;
			}
		}

		void releaseExecuting()
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_executing = false;
		}

		// Run a claimed batch and release the claim afterwards, whatever
		// happens.
		void runClaimed(const std::vector<Entry>& batch, const Runner& run)
		{
			runBatch(batch, run);
			releaseExecuting();
		}

		// Caller holds m_mutex.
		void syncHintLocked()
		{
			m_depthHint.store(m_pending.size(), std::memory_order_relaxed);
		}

		mutable std::mutex       m_mutex;
		std::recursive_mutex     m_exec;
		std::atomic<std::size_t> m_depthHint{ 0 };
		std::vector<Entry>       m_pending;
		Stats                    m_stats;
		std::size_t              m_capacity;
		std::size_t              m_maxIds;
		bool                     m_executing = false;
		bool                     m_shuttingDown = false;
	};

	// The exact teardown sequence, shared by the client and its regression
	// test: the queue is closed only on a real teardown, and only once.
	// Returns true when this call actually closed it.
	inline bool applyTeardown(PlacementGate& gate, Queue& queue)
	{
		if (!gate.claimTeardown())
			return false;
		queue.shutdown();
		return true;
	}
}
