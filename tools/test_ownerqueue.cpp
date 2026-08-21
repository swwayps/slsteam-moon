// Unit tests for the bounded owner-thread command queue
// (src/utils/ownerqueue.hpp) and its policy helpers (src/ownerwork.hpp).
//
// The queue exists so watcher-originated Steam-owned work (package-0
// injection via CUtlMemoryGrow, the NotifyLicensesUpdated broadcast, the API
// install call) can execute on the latched owner IPC thread instead of on an
// inotify pthread. This is defensive thread-affinity HARDENING; no causal
// claim about any client failure is made or tested here.
//
// What must hold:
//   * non-blocking        -> enqueue returns immediately; no wait on the owner
//   * bounded             -> overflow is rejected, never silently dropped
//   * coalescing          -> redundant work merges without changing the result
//   * idempotent          -> re-enqueuing pending work is a no-op
//   * exactly once        -> owner drain / stale flush / inline run, never two
//   * in order            -> claims are whole-queue, fallbacks run pending first
//   * serialised          -> two threads never execute this work concurrently
//   * stale escape hatch  -> if the owner never drains, the work still runs
//   * teardown gate       -> a PRE-HOOK Hooks::remove() leaves the queue
//                            accepting work; a real teardown closes it
//   * exception barrier   -> a throwing command cannot escape the drain
//
// Build (from repo root):
//   g++ -std=c++20 -pthread tools/test_ownerqueue.cpp -o /tmp/test_ownerqueue
//   /tmp/test_ownerqueue

#include "../src/utils/ownerqueue.hpp"
#include "../src/feats/hotreload_types.hpp"
// Only the inline policy helpers are used from this header (no Steam deps).
#include "../src/ownerwork.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                      \
	do {                                                                      \
		++g_checks;                                                           \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }         \
		else         { std::printf("ok:   %s\n", msg); }                       \
	} while (0)

using OwnerQueue::Command;
using OwnerQueue::Kind;
using OwnerQueue::PushResult;
using OwnerQueue::Queue;
using ::PackageSnapshot;

// A fixed, explicit "now" so nothing in these tests has to sleep.
static constexpr std::uint64_t kT0 = 1'000'000;

static void test_push_and_fifo_drain()
{
	std::printf("[1] enqueue + owner drain keeps FIFO order\n");
	Queue q;
	CHECK(q.push(Command::injectPackage0({ 10, 20 }), kT0) == PushResult::Queued,
	      "injection accepted");
	CHECK(q.push(Command::reconcileLicenses(), kT0) == PushResult::Queued,
	      "reconcile accepted");
	CHECK(q.depth() == 2, "both commands pending");
	CHECK(q.depthHint() == 2, "lock-free depth hint tracks the pending depth");

	std::vector<Kind> seen;
	const std::size_t drained = q.drain([&](const Command& c) { seen.push_back(c.kind()); });
	CHECK(drained == 2, "drain takes the whole batch");
	CHECK(seen.size() == 2 && seen[0] == Kind::InjectPackage0
	      && seen[1] == Kind::ReconcileLicenses,
	      "commands run in enqueue order (inject before reconcile)");
	CHECK(q.depth() == 0, "queue empty after drain");
	CHECK(q.depthHint() == 0, "depth hint cleared after drain (owner hot path sees no work)");
	CHECK(q.drain([](const Command&) {}) == 0, "drain of an empty queue is a no-op");
}

static void test_enqueue_does_not_block()
{
	std::printf("[2] enqueue is fire-and-forget (never waits for the owner)\n");
	Queue q;
	// No owner-side drainer exists at all here: a blocking handoff would hang
	// or burn its deadline. The measured owner cadence on the guest was
	// ~3.3-3.4 s, so anything above a few ms means we reintroduced a wait.
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < 50; ++i)
		q.pushBatch({ Command::injectPackage0({ static_cast<std::uint32_t>(i) }),
		              Command::reconcileLicenses() }, kT0);
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start);
	CHECK(waited < std::chrono::milliseconds(50),
	      "50 hot-add submits return without waiting for an owner frame");
	CHECK(q.depth() == 2, "all of it coalesced into one injection + one reconcile");
	CHECK(q.depthHint() == 2, "owner still sees the pending work");
}

static void test_coalescing()
{
	std::printf("[3] coalescing of redundant work\n");
	Queue q;
	CHECK(q.push(Command::injectPackage0({ 1, 2 }), kT0) == PushResult::Queued,
	      "first injection queued");
	CHECK(q.push(Command::injectPackage0({ 2, 3 }), kT0 + 5000) == PushResult::Coalesced,
	      "second injection coalesces into the pending one");
	CHECK(q.depth() == 1, "still a single pending injection");

	std::vector<std::uint32_t> ids;
	q.drain([&](const Command& c) { ids = c.appIds(); });
	const std::vector<std::uint32_t> expected{ 1, 2, 3 };
	CHECK(ids == expected,
	      "merged id list is the first-seen-order union (same result as running both)");

	// A reconcile is a single broadcast: a second pending one is redundant.
	CHECK(q.push(Command::reconcileLicenses(), kT0) == PushResult::Queued, "reconcile queued");
	CHECK(q.push(Command::reconcileLicenses(), kT0) == PushResult::Duplicate,
	      "second pending reconcile is redundant");
	CHECK(q.depth() == 1, "reconcile not duplicated");
	q.drain([](const Command&) {});
}

static void test_package_snapshot_last_write_wins()
{
	std::printf("[4a] package snapshots coalesce by generation (last write wins)\n");
	const PackageSnapshot a{
		1, { 10, 20 }, { 110, 220 }, true, { 10, 20 }, { 10, 20 }
	};
	const PackageSnapshot b{
		2, { 20, 30 }, { 220, 330 }, false, { 30 }, { 30 }
	};

	Queue q;
	CHECK(q.push(Command::syncPackage0(a), kT0) == PushResult::Queued,
	      "first package snapshot queues");
	CHECK(q.push(Command::syncPackage0(b), kT0 + 10) == PushResult::Coalesced,
	      "newer package snapshot replaces the pending payload");
	CHECK(q.depth() == 1, "snapshot replacement keeps one pending record");
	CHECK(q.oldestAgeUs(kT0 + 100) == 100,
	      "snapshot replacement preserves the oldest enqueue timestamp");

	PackageSnapshot received;
	q.drain([&](const Command& cmd)
	{
		CHECK(cmd.kind() == Kind::SyncPackage0,
		      "owner receives a SyncPackage0 command");
		received = cmd.packageSnapshot();
	});
	PackageSnapshot carriedB = b;
	carriedB.addedAppIds = { 20, 30 };
	carriedB.appInfoRequestIds = { 20, 30 };
	CHECK(received == carriedB,
	      "owner receives the latest snapshot with still-relevant additions carried forward");
	CHECK(received.appIds == std::vector<std::uint32_t>({ 20, 30 }) &&
	      received.depotIds == std::vector<std::uint32_t>({ 220, 330 }) &&
	      received.addedAppIds == std::vector<std::uint32_t>({ 20, 30 }) &&
	      received.appInfoRequestIds ==
	          std::vector<std::uint32_t>({ 20, 30 }) &&
	      !received.metadataComplete,
	      "latest snapshot preserves every still-relevant appinfo request");

	Queue repeat;
	CHECK(repeat.push(Command::syncPackage0(a), kT0) == PushResult::Queued,
	      "repeat test snapshot queues");
	CHECK(repeat.push(Command::syncPackage0(a), kT0 + 10) == PushResult::Duplicate,
	      "an exact repeated snapshot reports Duplicate and does not replace");
	CHECK(repeat.depth() == 1 && repeat.oldestAgeUs(kT0 + 100) == 100,
	      "an exact repeat keeps one record and its original timestamp");

	Queue sameGeneration;
	const PackageSnapshot conflicting{ 1, { 99 }, { 999 }, false, {}, {} };
	std::vector<PushResult> perCommand;
	CHECK(sameGeneration.pushBatch({ Command::syncPackage0(a) }, kT0,
	                               &perCommand),
	      "same-generation baseline batch is accepted");
	CHECK(perCommand.size() == 1 && perCommand[0] == PushResult::Queued,
	      "baseline batch reports Queued after its dry run and live apply");
	CHECK(sameGeneration.pushBatch({ Command::syncPackage0(conflicting) },
	                               kT0 + 10, &perCommand),
	      "conflicting equal-generation batch is accepted as idempotent");
	CHECK(perCommand.size() == 1 && perCommand[0] == PushResult::Duplicate,
	      "conflicting equal-generation batch reports Duplicate");
	CHECK(sameGeneration.depth() == 1 &&
	      sameGeneration.oldestAgeUs(kT0 + 100) == 100,
	      "equal-generation disagreement keeps one record and its original timestamp");
	PackageSnapshot sameGenerationRetained;
	sameGeneration.drain([&](const Command& cmd)
	{
		sameGenerationRetained = cmd.packageSnapshot();
	});
	CHECK(sameGenerationRetained == a,
	      "equal-generation disagreement leaves the original payload exact");

	Queue older;
	CHECK(older.push(Command::syncPackage0(b), kT0) == PushResult::Queued,
	      "newer snapshot queues for stale-generation test");
	CHECK(older.push(Command::syncPackage0(a), kT0 + 10) == PushResult::Duplicate,
	      "an older generation reports Duplicate and cannot replace newer work");
	PackageSnapshot retained;
	older.drain([&](const Command& cmd) { retained = cmd.packageSnapshot(); });
	CHECK(retained == b, "older-generation rejection leaves the newer payload intact");

	Queue order;
	std::vector<Kind> kinds;
	PackageSnapshot orderedSnapshot;
	CHECK(order.push(Command::injectPackage0({ 7 }), kT0) == PushResult::Queued,
	      "inject queues before a managed-state command");
	CHECK(order.push(Command::syncPackage0(a), kT0) == PushResult::Queued,
	      "managed-state command queues in FIFO position");
	CHECK(order.push(Command::reconcileLicenses(), kT0) == PushResult::Queued,
	      "reconcile queues after the managed-state command");
	CHECK(order.push(Command::syncPackage0(b), kT0 + 10) == PushResult::Coalesced,
	      "replacement does not move the managed-state FIFO position");
	CHECK(order.push(Command::installApp(9, 0), kT0) == PushResult::Queued,
	      "install queues after other command kinds");
	CHECK(order.drain([&](const Command& cmd)
	{
		kinds.push_back(cmd.kind());
		if (cmd.kind() == Kind::SyncPackage0)
			orderedSnapshot = cmd.packageSnapshot();
	}) == 4, "mixed command queue drains all records");
	CHECK(kinds == std::vector<Kind>({ Kind::InjectPackage0, Kind::SyncPackage0,
	                                   Kind::ReconcileLicenses, Kind::InstallApp }),
	      "snapshot replacement preserves FIFO position relative to other kinds");
	CHECK(orderedSnapshot == carriedB,
	      "the FIFO snapshot record carries the latest payload and pending additions");
}

static void test_idempotent_enqueue()
{
	std::printf("[4] idempotent enqueue\n");
	Queue q;
	q.push(Command::injectPackage0({ 7, 8 }), kT0);
	CHECK(q.push(Command::injectPackage0({ 7, 8 }), kT0) == PushResult::Duplicate,
	      "identical injection is a no-op");
	CHECK(q.push(Command::injectPackage0({ 8 }), kT0) == PushResult::Duplicate,
	      "subset injection adds nothing new");
	CHECK(q.depth() == 1, "no extra pending record");

	q.push(Command::installApp(480, 0), kT0);
	CHECK(q.push(Command::installApp(480, 0), kT0) == PushResult::Duplicate,
	      "identical install is a no-op");
	CHECK(q.push(Command::installApp(480, 1), kT0) == PushResult::Queued,
	      "install into a different library is distinct work");
	CHECK(q.depth() == 3, "one injection + two installs pending");
	CHECK(q.stats().duplicates == 3, "duplicate pushes counted");
}

static void test_bounded()
{
	std::printf("[5] bounded queue and bounded command\n");
	Queue q(2);
	CHECK(q.push(Command::installApp(1, 0), kT0) == PushResult::Queued, "1st install fits");
	CHECK(q.push(Command::installApp(2, 0), kT0) == PushResult::Queued, "2nd install fits");
	CHECK(q.push(Command::installApp(3, 0), kT0) == PushResult::Full,
	      "3rd install rejected at capacity (caller runs it itself)");
	CHECK(q.depth() == 2, "capacity never exceeded");
	CHECK(q.stats().rejectedFull == 1, "overflow rejection counted");

	// Coalescing still works at capacity: it adds no record.
	Queue small(1);
	CHECK(small.push(Command::injectPackage0({ 5 }), kT0) == PushResult::Queued,
	      "injection fits");
	CHECK(small.push(Command::injectPackage0({ 6 }), kT0) == PushResult::Coalesced,
	      "coalescing is allowed at capacity (no new record)");
	CHECK(small.push(Command::reconcileLicenses(), kT0) == PushResult::Full,
	      "a different kind is rejected at capacity");

	// Per-command id cap.
	Queue capped(8, 4);
	CHECK(capped.push(Command::injectPackage0({ 1, 2, 3, 4 }), kT0) == PushResult::Queued,
	      "id list at the cap is accepted");
	CHECK(capped.push(Command::injectPackage0({ 5 }), kT0) == PushResult::Full,
	      "merge past the id cap is rejected, pending record untouched");
	std::vector<std::uint32_t> ids;
	capped.drain([&](const Command& c) { ids = c.appIds(); });
	const std::vector<std::uint32_t> expected{ 1, 2, 3, 4 };
	CHECK(ids == expected, "rejected merge left the pending command unchanged");

	Queue capped2(8, 2);
	CHECK(capped2.push(Command::injectPackage0({ 1, 2, 3 }), kT0) == PushResult::Full,
	      "oversized command rejected outright");
	CHECK(capped2.depth() == 0, "nothing queued from a rejected command");

	Queue snapshotAppCap(8, 3);
	CHECK(snapshotAppCap.push(Command::syncPackage0(
		PackageSnapshot{ 1, { 1, 2, 3 }, {}, true, {}, {} }), kT0) == PushResult::Queued,
	      "snapshot app-id list at the cap is accepted");
	CHECK(snapshotAppCap.push(Command::syncPackage0(
		PackageSnapshot{ 2, { 1, 2, 3, 4 }, {}, true, {}, {} }), kT0 + 1) == PushResult::Full,
	      "snapshot app-id list beyond the cap is rejected");

	Queue snapshotDepotCap(8, 3);
	CHECK(snapshotDepotCap.push(Command::syncPackage0(
		PackageSnapshot{ 1, {}, { 11, 22, 33 }, true, {}, {} }), kT0) == PushResult::Queued,
	      "snapshot depot-id list at the cap is accepted");
	CHECK(snapshotDepotCap.push(Command::syncPackage0(
		PackageSnapshot{ 2, {}, { 11, 22, 33, 44 }, true, {}, {} }), kT0 + 1) == PushResult::Full,
	      "snapshot depot-id list beyond the cap is rejected");
	CHECK(snapshotDepotCap.depth() == 1,
	      "a rejected snapshot bound leaves the pending record unchanged");

	Queue snapshotAddedCap(8, 2);
	CHECK(snapshotAddedCap.push(Command::syncPackage0(
		PackageSnapshot{ 1, { 1 }, { 11 }, true, { 1, 2, 3 }, {} }), kT0) ==
		      PushResult::Full,
	      "snapshot added-id list beyond the cap is rejected");

	Queue snapshotAppInfoRequestCap(8, 2);
	CHECK(snapshotAppInfoRequestCap.push(Command::syncPackage0(
		PackageSnapshot{ 1, { 1 }, { 11 }, true, { 1 }, { 1, 2, 3 } }), kT0) ==
		      PushResult::Full,
	      "snapshot appinfo request list beyond the cap is rejected");

	Queue snapshotCapacity(1);
	CHECK(snapshotCapacity.push(Command::syncPackage0(
		PackageSnapshot{ 1, { 1 }, { 11 }, true, {}, {} }), kT0) == PushResult::Queued,
	      "a snapshot occupies one capacity slot");
	CHECK(snapshotCapacity.push(Command::syncPackage0(
		PackageSnapshot{ 2, { 2 }, { 22 }, true, {}, {} }), kT0 + 1) == PushResult::Coalesced,
	      "a newer snapshot still coalesces when the queue is at capacity");
	CHECK(snapshotCapacity.push(Command::installApp(5, 0), kT0 + 2) == PushResult::Full,
	      "a different command kind is rejected at snapshot capacity");
}

static void test_shutdown_rejection()
{
	std::printf("[6] shutdown rejection\n");
	Queue q;
	q.push(Command::injectPackage0({ 42 }), kT0);
	q.push(Command::reconcileLicenses(), kT0);
	q.shutdown();
	CHECK(q.isShuttingDown(), "queue reports shutdown");
	CHECK(q.depth() == 0, "pending work abandoned at shutdown");
	CHECK(q.stats().abandoned == 2, "abandoned commands counted");
	CHECK(q.push(Command::reconcileLicenses(), kT0) == PushResult::ShuttingDown,
	      "push after shutdown is rejected");
	CHECK(q.pushBatch({ Command::reconcileLicenses() }, kT0) == false,
	      "batch push after shutdown is rejected");
	CHECK(q.stats().rejectedShutdown == 2, "shutdown rejections counted");
	CHECK(q.drain([](const Command&) {}) == 0, "nothing to drain after shutdown");
	CHECK(q.flushStale(kT0 + 60'000'000, 1000, [](const Command&) {}) == 0,
	      "the stale escape hatch does not run work during teardown");

	Queue snapshot;
	snapshot.push(Command::syncPackage0(
		PackageSnapshot{ 1, { 7 }, { 77 }, true, {}, {} }), kT0);
	snapshot.shutdown();
	CHECK(snapshot.push(Command::syncPackage0(
		PackageSnapshot{ 2, { 8 }, { 88 }, true, {}, {} }), kT0) == PushResult::ShuttingDown,
	      "a snapshot is rejected after shutdown");
	CHECK(snapshot.depth() == 0 && snapshot.stats().abandoned == 1,
	      "snapshot work is abandoned at shutdown");
}

// Problem 1 regression. main.cpp's load() runs once per audited module open,
// and its "the other module isn't mapped yet" retry goes
// load() -> unload() -> Hooks::remove() BEFORE anything is hooked. If that
// closes the queue, every watcher hot-add for the rest of the session is
// abandoned: no package-0 injection, no license broadcast, whole session.
static void test_teardown_gate()
{
	std::printf("[7] teardown gate: pre-hook remove() must NOT close the queue\n");
	OwnerQueue::PlacementGate gate;
	Queue q;

	// --- first la_objopen: steamclient.so maps, steamui.so is not there yet,
	// load() bails through unload() -> Hooks::remove(). No placement happened.
	CHECK(!gate.placed(), "no placement recorded before the hooking pass");
	CHECK(!OwnerQueue::applyTeardown(gate, q),
	      "pre-hook teardown does not close the queue");
	CHECK(!q.isShuttingDown(), "queue still open after the benign retry path");
	CHECK(q.push(Command::injectPackage0({ 480 }), kT0) == PushResult::Queued,
	      "a pre-hook Hooks::remove() leaves the queue accepting work");

	// Several retries in a row must stay harmless.
	for (int i = 0; i < 3; ++i)
		CHECK(!OwnerQueue::applyTeardown(gate, q), "repeated pre-hook teardown stays a no-op");
	CHECK(q.push(Command::reconcileLicenses(), kT0) == PushResult::Queued,
	      "queue still accepts work after repeated pre-hook teardowns");
	CHECK(q.drain([](const Command&) {}) == 2, "the queued work is still drainable");

	// --- second la_objopen: both modules mapped, the hooking pass runs.
	gate.notePlacement();
	CHECK(gate.placed(), "placement recorded");
	CHECK(q.push(Command::injectPackage0({ 480 }), kT0) == PushResult::Queued,
	      "queue accepts work while hooks are live");

	// --- real teardown.
	CHECK(OwnerQueue::applyTeardown(gate, q), "a real teardown closes the queue");
	CHECK(q.isShuttingDown(), "queue reports shutdown after a real teardown");
	CHECK(q.depth() == 0, "pending work abandoned by the real teardown");
	CHECK(q.push(Command::reconcileLicenses(), kT0) == PushResult::ShuttingDown,
	      "nothing is accepted after a real teardown");
	CHECK(!OwnerQueue::applyTeardown(gate, q), "a repeated teardown is a no-op");
	CHECK(q.isShuttingDown(), "the queue stays closed");
}

static void test_exception_containment()
{
	std::printf("[8] exception containment during drain\n");
	Queue q;
	q.push(Command::injectPackage0({ 1 }), kT0);
	q.push(Command::reconcileLicenses(), kT0);
	q.push(Command::installApp(9, 0), kT0);

	int ran = 0;
	std::size_t drained = 0;
	bool escaped = false;
	try
	{
		drained = q.drain([&](const Command& c)
		{
			++ran;
			if (c.kind() == Kind::ReconcileLicenses)
				throw std::runtime_error("simulated Steam-side failure");
		});
	}
	catch (...)
	{
		escaped = true;
	}
	CHECK(!escaped, "no exception escapes drain into the IPC frame");
	CHECK(drained == 3, "all commands taken");
	CHECK(ran == 3, "a throwing command does not skip the rest");
	CHECK(q.stats().failed == 1 && q.stats().executed == 2,
	      "failure and success counted separately");
	CHECK(q.depth() == 0, "queue drained despite the failure");

	// The queue must stay usable after a failed command.
	CHECK(q.push(Command::reconcileLicenses(), kT0) == PushResult::Queued,
	      "queue still accepts work after a failure");

	// The inline fallback needs the same barrier: it runs on a watcher thread.
	Queue q2;
	bool escaped2 = false;
	try
	{
		q2.runPendingThen({ Command::reconcileLicenses() },
		                  [](const Command&) { throw std::runtime_error("boom"); });
	}
	catch (...)
	{
		escaped2 = true;
	}
	CHECK(!escaped2, "no exception escapes the inline fallback into the watcher callback");
	CHECK(q2.stats().failed == 1, "inline failure counted");
}

static void test_reentrancy_guard()
{
	std::printf("[9] drain is not re-entrant\n");
	Queue q;
	q.push(Command::injectPackage0({ 1 }), kT0);
	std::size_t inner = 1;
	q.drain([&](const Command&)
	{
		// A nested owner frame must not start a second drain.
		q.push(Command::reconcileLicenses(), kT0);
		inner = q.drain([](const Command&) {});
	});
	CHECK(inner == 0, "nested drain takes nothing");
	CHECK(q.depth() == 1, "work pushed from inside a drain stays pending");
}

// Problem 2's escape hatch. Fire-and-forget means nobody is waiting, so the
// only protection against "the owner never runs another IPC frame" is this.
static void test_stale_escape_hatch()
{
	std::printf("[10] bounded-staleness escape hatch (owner never drains)\n");
	CHECK(!OwnerQueue::isStale(kT0, kT0 + 1'000'000, 30'000'000),
	      "1 s old with a 30 s threshold is not stale");
	CHECK(OwnerQueue::isStale(kT0, kT0 + 30'000'000, 30'000'000),
	      "exactly at the threshold is stale");
	CHECK(!OwnerQueue::isStale(kT0, kT0 + 999'000'000, 0),
	      "threshold 0 means never escape (eventual execution accepted)");
	CHECK(!OwnerQueue::isStale(kT0, kT0 - 5, 1000), "a backwards clock never triggers it");

	Queue q;
	q.pushBatch({ Command::injectPackage0({ 480 }), Command::reconcileLicenses() }, kT0);

	std::vector<Kind> ran;
	const auto runner = [&](const Command& c) { ran.push_back(c.kind()); };

	CHECK(q.flushStale(kT0 + 3'400'000, 30'000'000, runner) == 0,
	      "at the measured 3.4 s owner cadence nothing is flushed");
	CHECK(ran.empty(), "work still waiting for the owner");
	CHECK(q.depth() == 2, "still pending");

	const std::size_t flushed = q.flushStale(kT0 + 30'000'000, 30'000'000, runner);
	CHECK(flushed == 2, "past the threshold the whole pending set is flushed");
	CHECK(ran.size() == 2 && ran[0] == Kind::InjectPackage0
	      && ran[1] == Kind::ReconcileLicenses,
	      "the escape hatch preserves the original order");
	CHECK(q.depth() == 0, "flushed work is no longer pending (runs exactly once)");
	CHECK(q.stats().flushedStale == 2, "stale flush counted");
	CHECK(q.flushStale(kT0 + 60'000'000, 30'000'000, runner) == 0,
	      "nothing left to flush");

	// Coalescing must not be able to postpone the escape hatch for ever: the
	// merged record keeps the ORIGINAL enqueue time.
	Queue q2;
	q2.push(Command::injectPackage0({ 1 }), kT0);
	for (int i = 2; i < 40; ++i)
		q2.push(Command::injectPackage0({ static_cast<std::uint32_t>(i) }),
		        kT0 + static_cast<std::uint64_t>(i) * 1'000'000);
	CHECK(q2.depth() == 1, "all those injections coalesced");
	std::size_t merged = 0;
	CHECK(q2.flushStale(kT0 + 30'000'000, 30'000'000,
	                    [&](const Command& c) { merged = c.appIds().size(); }) == 1,
	      "a stream of coalescing pushes cannot starve the escape hatch");
	CHECK(merged == 39, "the flushed command carries every merged id");

	// The age reported for diagnostics tracks the oldest entry.
	Queue q3;
	q3.push(Command::reconcileLicenses(), kT0);
	CHECK(q3.oldestAgeUs(kT0 + 2'500'000) == 2'500'000, "oldest age reported for logging");
	CHECK(Queue().oldestAgeUs(kT0) == 0, "empty queue reports no age");
}

static void test_inline_fallback_ordering()
{
	std::printf("[11] inline fallbacks run pending work first (order preserved)\n");
	Queue q(2);
	// Two installs are pending and the queue is full, so the next batch is
	// rejected and must run inline — but AFTER the work that was queued first.
	q.push(Command::installApp(1, 0), kT0);
	q.push(Command::installApp(2, 0), kT0);
	CHECK(q.pushBatch({ Command::installApp(3, 0) }, kT0) == false,
	      "the third install does not fit");

	std::vector<std::uint32_t> order;
	const std::size_t ran = q.runPendingThen({ Command::installApp(3, 0) },
	                                        [&](const Command& c) { order.push_back(c.appId()); });
	CHECK(ran == 3, "pending work plus the rejected batch all ran");
	const std::vector<std::uint32_t> expected{ 1, 2, 3 };
	CHECK(order == expected, "queued work ran before the overflow batch, in order");
	CHECK(q.depth() == 0, "nothing left pending");
	CHECK(q.stats().ranInline == 3, "inline executions counted");

	// With nothing pending it is just the batch.
	Queue q2;
	std::vector<std::uint32_t> order2;
	CHECK(q2.runPendingThen({ Command::installApp(9, 0) },
	                       [&](const Command& c) { order2.push_back(c.appId()); }) == 1,
	      "inline run with an empty queue runs only the given batch");
	CHECK(order2.size() == 1 && order2[0] == 9, "and runs it unchanged");
	CHECK(q2.runPendingThen({}, [](const Command&) {}) == 0, "empty inline run is a no-op");
}

static void test_serialised_execution()
{
	std::printf("[12] Steam-owned work is never executed by two threads at once\n");
	Queue q;
	std::atomic<int> concurrent{ 0 };
	std::atomic<int> maxConcurrent{ 0 };
	std::atomic<int> executions{ 0 };
	std::atomic<bool> stop{ false };

	const auto runner = [&](const Command&)
	{
		const int now = concurrent.fetch_add(1) + 1;
		int prev = maxConcurrent.load();
		while (now > prev && !maxConcurrent.compare_exchange_weak(prev, now))
			;
		std::this_thread::sleep_for(std::chrono::microseconds(50));
		executions.fetch_add(1);
		concurrent.fetch_sub(1);
	};

	// One thread plays the owner IPC frame, one plays a watcher hitting the
	// stale escape hatch, one plays a watcher forced inline (queue disabled).
	std::thread owner([&] { while (!stop.load()) q.drain(runner); });
	std::thread sweeper([&]
	{
		while (!stop.load())
			q.flushStale(OwnerQueue::monotonicUs(), 1, runner);
	});
	std::thread direct([&]
	{
		for (int i = 0; i < 200; ++i)
			q.runPendingThen({ Command::installApp(static_cast<std::uint32_t>(i), 7) }, runner);
	});

	for (int i = 0; i < 400; ++i)
		q.push(Command::installApp(static_cast<std::uint32_t>(i), 0), OwnerQueue::monotonicUs());

	direct.join();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	stop = true;
	owner.join();
	sweeper.join();
	q.drain(runner);

	CHECK(maxConcurrent.load() == 1,
	      "at most one thread inside the Steam-owned work at any time");
	CHECK(q.depth() == 0, "everything was executed");
	const auto s = q.stats();
	CHECK(s.executed + s.failed == static_cast<std::uint64_t>(executions.load()),
	      "queue accounting matches the number of executions");
}

static void test_push_batch_atomicity()
{
	std::printf("[13] batch enqueue is all-or-nothing\n");
	Queue q(2);
	std::vector<PushResult> per;
	const bool ok = q.pushBatch({ Command::injectPackage0({ 1 }),
	                              Command::reconcileLicenses() }, kT0, &per);
	CHECK(ok, "batch that fits is accepted");
	CHECK(per.size() == 2 && per[0] == PushResult::Queued && per[1] == PushResult::Queued,
	      "per-command results reported");

	// Capacity is full: a batch needing a third record must be rejected whole.
	const bool rejected = q.pushBatch({ Command::injectPackage0({ 2 }),
	                                    Command::installApp(5, 0) }, kT0, &per);
	CHECK(!rejected, "batch that does not fit is rejected");
	CHECK(q.depth() == 2, "rejected batch leaves the queue untouched");
	std::vector<std::uint32_t> ids;
	q.drain([&](const Command& c)
	{
		if (c.kind() == Kind::InjectPackage0) ids = c.appIds();
	});
	const std::vector<std::uint32_t> expected{ 1 };
	CHECK(ids == expected, "rejected batch did not partially coalesce");
}

static void test_owner_affinity_gate()
{
	std::printf("[14] owner-thread affinity gate (pure)\n");
	// Only the latched owner thread, with work pending and not already inside a
	// drain, may run Steam-owned work.
	CHECK(OwnerQueue::shouldDrain(100, 100, false, 1), "owner thread with work drains");
	CHECK(!OwnerQueue::shouldDrain(100, 100, false, 0), "no work -> no drain");
	CHECK(!OwnerQueue::shouldDrain(101, 100, false, 1), "non-owner thread never drains");
	CHECK(!OwnerQueue::shouldDrain(100, 0, false, 1), "no owner latched -> no drain");
	CHECK(!OwnerQueue::shouldDrain(100, 100, true, 1), "already draining -> no nested drain");
}

static void test_policy_parsing()
{
	std::printf("[15] handoff policy parsing (pure)\n");
	using OwnerWork::parseBoundedMs;
	using OwnerWork::parseQueueEnabled;
	bool rejected = false;

	CHECK(parseBoundedMs(nullptr, 30000, 600000, &rejected) == 30000 && !rejected,
	      "unset staleness -> default, not reported as invalid");
	CHECK(parseBoundedMs("", 30000, 600000, &rejected) == 30000 && !rejected,
	      "empty -> default");
	CHECK(parseBoundedMs("45000", 30000, 600000, &rejected) == 45000 && !rejected,
	      "valid override honoured");
	CHECK(parseBoundedMs("0", 30000, 600000, &rejected) == 0 && !rejected,
	      "0 is valid: accept eventual execution, never escape inline");
	CHECK(parseBoundedMs("600000", 30000, 600000, &rejected) == 600000 && !rejected,
	      "the maximum is accepted");
	CHECK(parseBoundedMs("600001", 30000, 600000, &rejected) == 30000 && rejected,
	      "above the maximum is refused");
	CHECK(parseBoundedMs("99999999999999999999", 30000, 600000, &rejected) == 30000 && rejected,
	      "overflowing input is refused without wrapping");
	CHECK(parseBoundedMs("30s", 30000, 600000, &rejected) == 30000 && rejected,
	      "trailing garbage is refused");
	CHECK(parseBoundedMs("-5", 30000, 600000, &rejected) == 30000 && rejected,
	      "negative input is refused");

	CHECK(parseQueueEnabled(nullptr, true, &rejected) && !rejected,
	      "queue is on by default");
	CHECK(!parseQueueEnabled("0", true, &rejected) && !rejected, "0 disables the queue");
	CHECK(!parseQueueEnabled("OFF", true, &rejected) && !rejected, "off disables it");
	CHECK(!parseQueueEnabled("no", true, &rejected) && !rejected, "no disables it");
	CHECK(parseQueueEnabled("1", true, &rejected) && !rejected, "1 keeps it on");
	CHECK(parseQueueEnabled("true", true, &rejected) && !rejected, "true keeps it on");
	CHECK(parseQueueEnabled("banana", true, &rejected) && rejected,
	      "an unrecognised value must NOT silently disable hardening");

	// The measured owner cadence must sit comfortably inside the default
	// staleness window, otherwise the escape hatch would fire on a healthy
	// client and we would be back to off-owner execution.
	CHECK(OwnerWork::kDefaultMaxStaleMs >= 5 * 3400,
	      "default staleness threshold is far above the measured 3.4 s cadence");
}

static void test_exactly_once_under_race()
{
	std::printf("[16] each command executes exactly once under owner/watcher race\n");
	constexpr int kRounds = 300;
	// Capacity for every round, so this test measures the exactly-once
	// property and not the (separately tested) overflow rejection.
	Queue q(kRounds + 4);
	std::vector<std::atomic<int>> runs(kRounds);
	for (auto& r : runs)
		r.store(0);
	std::atomic<int> ownerRuns{ 0 };
	std::atomic<int> staleRuns{ 0 };
	std::atomic<bool> stop{ false };

	const auto record = [&](const Command& c) { runs[c.appId()].fetch_add(1); };

	std::thread owner([&]
	{
		while (!stop.load())
			ownerRuns.fetch_add(static_cast<int>(q.drain(record)));
	});
	// A second thread races the escape hatch with a 1 us threshold, i.e. the
	// most hostile possible interleaving.
	std::thread sweeper([&]
	{
		while (!stop.load())
		{
			staleRuns.fetch_add(static_cast<int>(
				q.flushStale(OwnerQueue::monotonicUs(), 1, record)));
		}
	});

	int accepted = 0;
	for (int i = 0; i < kRounds; ++i)
	{
		if (q.push(Command::installApp(static_cast<std::uint32_t>(i), 0),
		           OwnerQueue::monotonicUs()) == PushResult::Queued)
			++accepted;
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	stop = true;
	owner.join();
	sweeper.join();
	ownerRuns.fetch_add(static_cast<int>(q.drain(record)));

	CHECK(accepted == kRounds, "every distinct command was accepted");
	CHECK(q.depth() == 0, "nothing left pending");
	int wrong = 0;
	for (int i = 0; i < kRounds; ++i)
		if (runs[i].load() != 1)
			++wrong;
	CHECK(wrong == 0, "every command ran exactly once: no loss, no double execution");
	CHECK(ownerRuns.load() + staleRuns.load() == kRounds,
	      "owner drain and stale flush together account for all of it");
	CHECK(ownerRuns.load() > 0 && staleRuns.load() > 0,
	      "both execution sites really raced (the test is not degenerate)");
}

int main()
{
	test_push_and_fifo_drain();
	test_enqueue_does_not_block();
	test_coalescing();
	test_package_snapshot_last_write_wins();
	test_idempotent_enqueue();
	test_bounded();
	test_shutdown_rejection();
	test_teardown_gate();
	test_exception_containment();
	test_reentrancy_guard();
	test_stale_escape_hatch();
	test_inline_fallback_ordering();
	test_serialised_execution();
	test_push_batch_atomicity();
	test_owner_affinity_gate();
	test_policy_parsing();
	test_exactly_once_under_race();

	if (g_failures == 0)
	{
		std::printf("\ntest_ownerqueue: ALL PASS (%d checks)\n", g_checks);
		return 0;
	}
	std::printf("\ntest_ownerqueue: %d/%d CHECK(S) FAILED\n", g_failures, g_checks);
	return 1;
}
