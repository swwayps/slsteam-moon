// Unit tests for the toggleable thread-affinity trace (src/afftrace.hpp).
//
// The trace is DIAGNOSTICS, DISABLED BY DEFAULT. It exists so a future user
// report can answer the two questions a controlled VM run left open:
//   Q1 does a watcher-originated Steam-owned call ever run off the owner IPC
//      thread WHILE an owner IPC frame is in flight (or one starts during it)?
//   Q2 do those overlap counts grow per session hour?
//
// What must hold:
//   * off by default        -> unset opt-in writes nothing and creates no file
//   * explicit opt-in       -> SLSSTEAM_AFFTRACE=1 enables it
//   * 0600 + bounded        -> sink is user-only and stops at the record cap
//   * privacy               -> only the fixed field set is ever written
//   * hot path gated        -> IPC frame records only inside a call window
//   * Q1 answerable         -> off_owner + ipc_depth/frames_during captured
//   * Q2 answerable         -> per-hour rollup counters grow
//   * frame accounting      -> the guard now sits on EVERY hooked dispatcher,
//                              so it must count owner frames and ONLY owner
//                              frames, and a drain on the owner must show
//                              ipc_depth >= 1
//
// Build (from repo root):
//   g++ -std=c++20 -pthread tools/test_afftrace.cpp -o /tmp/test_afftrace
//   /tmp/test_afftrace

#include "../src/afftrace.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
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

static std::string g_dir;

static std::string tracePath() { return g_dir + "/.SLSsteam.afftrace.log"; }

static std::vector<std::string> readLines()
{
	std::vector<std::string> out;
	std::ifstream f(tracePath());
	std::string line;
	while (std::getline(f, line))
		if (!line.empty())
			out.push_back(line);
	return out;
}

static bool fileExists(const std::string& p)
{
	struct stat st{};
	return ::stat(p.c_str(), &st) == 0;
}

static void enableTracing()
{
	AffTrace::reset();
	::setenv("HOME", g_dir.c_str(), 1);
	::setenv("SLSSTEAM_AFFTRACE", "1", 1);
	::unlink(tracePath().c_str());
	CHECK(AffTrace::init(), "init() enables tracing with the opt-in set");
}

static void test_opt_in_parsing()
{
	std::printf("[1] opt-in parsing (pure)\n");
	CHECK(!AffTrace::optInEnabled(nullptr), "unset -> off");
	CHECK(!AffTrace::optInEnabled(""), "empty -> off");
	CHECK(!AffTrace::optInEnabled("0"), "0 -> off");
	CHECK(!AffTrace::optInEnabled("no"), "no -> off");
	CHECK(!AffTrace::optInEnabled("maybe"), "unrecognised -> off");
	CHECK(AffTrace::optInEnabled("1"), "1 -> on");
	CHECK(AffTrace::optInEnabled("YES"), "YES -> on");
	CHECK(AffTrace::optInEnabled("True"), "True -> on");
	CHECK(AffTrace::optInEnabled("on"), "on -> on");
}

static void test_record_format_and_privacy()
{
	std::printf("[2] record format + privacy (pure)\n");
	AffTrace::Record r;
	r.seq = 7;
	r.tMonoUs = 1234567;
	r.kind = AffTrace::Kind::SteamCallExit;
	r.src = AffTrace::Src::Config;
	r.call = AffTrace::Call::Package0Inject;
	r.mode = AffTrace::Mode::Queued;
	r.tid = 4242;
	r.ownerTid = 99;
	r.offOwner = 1;
	r.ipcDepth = 2;
	r.ipcFrames = 500;
	r.queueDepth = 3;
	r.durUs = 1361;
	r.framesDuring = 1;

	const std::string line = AffTrace::format(r);
	CHECK(line.back() == '\n', "record is one newline-terminated line");

	// The field set is fixed and closed: exactly these keys, nothing else.
	static const char* kKeys[] = {
		"seq", "t_mono_us", "kind", "src", "call", "mode", "tid", "owner_tid",
		"off_owner", "ipc_depth", "ipc_frames", "queue_depth", "dur_us",
		"frames_during",
	};
	std::istringstream ss(line);
	std::string tok;
	std::size_t fields = 0;
	bool allKnown = true;
	while (ss >> tok)
	{
		const auto eq = tok.find('=');
		if (eq == std::string::npos) { allKnown = false; break; }
		const std::string key = tok.substr(0, eq);
		bool known = false;
		for (const char* k : kKeys)
			if (key == k) { known = true; break; }
		if (!known) { allKnown = false; std::printf("      unexpected key: %s\n", key.c_str()); }
		++fields;
	}
	CHECK(allKnown, "no field outside the approved set");
	CHECK(fields == sizeof(kKeys) / sizeof(kKeys[0]), "every approved field present");
	CHECK(line.find("kind=STEAMCALL_EXIT") != std::string::npos, "symbolic kind label");
	CHECK(line.find("src=config") != std::string::npos, "symbolic source label");
	CHECK(line.find("call=package0_inject") != std::string::npos, "symbolic call label");
	CHECK(line.find("mode=queued") != std::string::npos, "symbolic mode label");
	CHECK(line.find("off_owner=1") != std::string::npos, "off-owner flag");

	const std::string rollup = AffTrace::formatRollup(
		AffTrace::Rollup{ 1, 2, 3, 4, 5, 6, 7, 8 });
	CHECK(rollup.find("kind=HOUR_ROLLUP hour=3") != std::string::npos,
	      "rollup line carries the hour index");
	CHECK(rollup.find("overlaps=7") != std::string::npos, "rollup line carries overlaps");

	// Execution-site labels the fire-and-forget handoff can report. The old
	// blocking-deadline label is gone; these two replace it.
	AffTrace::Record s = r;
	s.kind = AffTrace::Kind::QueueStale;
	s.mode = AffTrace::Mode::DirectStale;
	CHECK(AffTrace::format(s).find("kind=QUEUE_STALE") != std::string::npos,
	      "the staleness escape hatch has its own record kind");
	CHECK(AffTrace::format(s).find("mode=direct_stale") != std::string::npos,
	      "the staleness escape hatch is labelled, never silent");
	s.mode = AffTrace::Mode::DirectDisabled;
	CHECK(AffTrace::format(s).find("mode=direct_disabled") != std::string::npos,
	      "a switched-off queue is labelled too");
}

static void test_overlap_predicate()
{
	std::printf("[3] overlap predicate (Q1)\n");
	AffTrace::Record r;
	r.kind = AffTrace::Kind::SteamCallExit;
	r.offOwner = 1;
	CHECK(!AffTrace::isOverlap(r), "off-owner alone is not an overlap");
	r.ipcDepth = 1;
	CHECK(AffTrace::isOverlap(r), "off-owner with a frame in flight is an overlap");
	r.ipcDepth = 0;
	r.framesDuring = 1;
	CHECK(AffTrace::isOverlap(r), "off-owner with a frame starting during it is an overlap");
	r.offOwner = 0;
	CHECK(!AffTrace::isOverlap(r), "owner-thread execution is never an overlap");
	r.offOwner = 1;
	r.kind = AffTrace::Kind::SteamFnExit;
	CHECK(!AffTrace::isOverlap(r), "only the work-unit exit record counts");
}

static void test_disabled_by_default()
{
	std::printf("[4] disabled by default\n");
	AffTrace::reset();
	::setenv("HOME", g_dir.c_str(), 1);
	::unsetenv("SLSSTEAM_AFFTRACE");
	::unlink(tracePath().c_str());

	CHECK(!AffTrace::init(), "init() returns false without the opt-in");
	CHECK(!AffTrace::enabled(), "tracing reports disabled");

	AffTrace::latchOwner();
	AffTrace::ipcFrameEnter();
	AffTrace::ipcFrameExit();
	AffTrace::queuePush(AffTrace::Src::Config, AffTrace::Call::Package0Inject,
	                    AffTrace::Mode::Queued, 1);
	{
		auto watch = AffTrace::watchSpan(AffTrace::Src::Config);
		auto call = AffTrace::callSpan(AffTrace::Src::Config,
		                              AffTrace::Call::Package0Inject,
		                              AffTrace::Mode::Direct);
		auto fn = AffTrace::fnSpan(AffTrace::Call::CutlMemoryGrow,
		                           AffTrace::Mode::Direct);
	}
	CHECK(!fileExists(tracePath()), "no trace file is created when disabled");
	CHECK(AffTrace::ownerTid() == 0, "no owner TID latched when disabled");
	CHECK(AffTrace::ipcFrameCount() == 0, "no frame counting when disabled");

	// A second init() must not flip it on.
	CHECK(!AffTrace::init(), "repeat init() stays disabled");
}

static void test_enabled_sink()
{
	std::printf("[5] opt-in sink: permissions, content, gating\n");
	enableTracing();
	CHECK(AffTrace::enabled(), "tracing reports enabled");
	CHECK(fileExists(tracePath()), "sink file created");

	struct stat st{};
	::stat(tracePath().c_str(), &st);
	CHECK((st.st_mode & 07777) == 0600, "sink is 0600 (user read/write only)");

	AffTrace::latchOwner();
	CHECK(AffTrace::ownerTid() != 0, "owner TID latched");

	// Outside a Steam-call window an IPC frame must NOT be recorded (hot path
	// stays quiet) but must still be counted.
	const auto before = readLines().size();
	AffTrace::ipcFrameEnter();
	AffTrace::ipcFrameExit();
	CHECK(readLines().size() == before, "IPC frames outside a call window are not logged");
	CHECK(AffTrace::ipcFrameCount() == 1, "IPC frames are still counted");

	// Inside a call window they are recorded (through the RAII frame guard the
	// IPC hook uses, so depth accounting is exercised the same way).
	{
		auto call = AffTrace::callSpan(AffTrace::Src::Config,
		                              AffTrace::Call::Package0Inject,
		                              AffTrace::Mode::Queued);
		AffTrace::FrameGuard frame;
		CHECK(AffTrace::ipcDepth() == 1, "frame guard tracks IPC depth");
	}
	CHECK(AffTrace::ipcDepth() == 0, "frame guard restores IPC depth on scope exit");
	const auto lines = readLines();
	bool sawEnter = false, sawExit = false, sawCall = false;
	for (const auto& l : lines)
	{
		if (l.find("kind=IPC_ENTER") != std::string::npos) sawEnter = true;
		if (l.find("kind=IPC_EXIT") != std::string::npos) sawExit = true;
		if (l.find("kind=STEAMCALL_EXIT") != std::string::npos) sawCall = true;
	}
	CHECK(sawEnter && sawExit, "IPC frames inside a call window are recorded");
	CHECK(sawCall, "the Steam-owned work unit is recorded");
	bool ownerLine = false;
	for (const auto& l : lines)
		if (l.find("kind=IPC_OWNER") != std::string::npos) ownerLine = true;
	CHECK(ownerLine, "owner latch recorded once");
}

static void test_offowner_overlap_capture()
{
	std::printf("[6] Q1 evidence: off-owner Steam call overlapping an owner frame\n");
	enableTracing();
	AffTrace::latchOwner();  // this thread is the owner

	// A frame is in flight on the owner while another thread runs the call.
	AffTrace::ipcFrameEnter();
	std::thread watcher([]
	{
		auto watch = AffTrace::watchSpan(AffTrace::Src::Config);
		auto call = AffTrace::callSpan(AffTrace::Src::Config,
		                              AffTrace::Call::Package0Inject,
		                              AffTrace::Mode::Direct);
		auto fn = AffTrace::fnSpan(AffTrace::Call::CutlMemoryGrow,
		                           AffTrace::Mode::Direct);
	});
	watcher.join();
	AffTrace::ipcFrameExit();

	bool overlapLine = false;
	for (const auto& l : readLines())
	{
		if (l.find("kind=STEAMCALL_EXIT") != std::string::npos
		    && l.find("off_owner=1") != std::string::npos
		    && l.find("ipc_depth=0") == std::string::npos)
		{
			overlapLine = true;
		}
	}
	CHECK(overlapLine,
	      "trace shows off_owner=1 with an owner IPC frame in flight (the missing evidence)");
}

// The first VM run guarded only the IClientUtils dispatcher, so the drain that
// actually succeeded came from another dispatcher and was recorded with
// ipc_depth=0 — ipc_frames/ipc_depth, and therefore HOUR_ROLLUP.overlaps,
// under-reported real owner-frame activity. The guard is now on every hooked
// dispatcher, which only works if it counts owner frames and nothing else.
static void test_owner_frame_accounting()
{
	std::printf("[7] frame accounting covers every dispatcher, owner-thread only\n");
	enableTracing();
	AffTrace::latchOwner();  // this thread is the owner
	CHECK(AffTrace::onOwnerThread(), "the latching thread is the owner");

	const std::uint64_t base = AffTrace::ipcFrameCount();

	// A dispatcher running on some other thread must not pollute the
	// owner-frame counters.
	std::thread other([&]
	{
		CHECK(!AffTrace::onOwnerThread(), "another thread is not the owner");
		AffTrace::FrameGuard frame;
		CHECK(!frame.counting(), "a non-owner dispatcher frame is not counted");
	});
	other.join();
	CHECK(AffTrace::ipcFrameCount() == base,
	      "owner frame count untouched by a non-owner dispatcher");
	CHECK(AffTrace::ipcDepth() == 0, "owner frame depth untouched by a non-owner dispatcher");

	// Three different dispatchers running on the owner thread each account
	// their frame, and a drain inside one of them is recorded INSIDE the frame.
	for (int i = 0; i < 3; ++i)
	{
		AffTrace::FrameGuard frame;
		CHECK(frame.counting(), "an owner-thread dispatcher frame is counted");
		if (i == 2)
		{
			CHECK(AffTrace::ipcDepth() == 1, "the drain runs inside an owner IPC frame");
			AffTrace::queueDrain(2, 0);
			auto call = AffTrace::callSpan(AffTrace::Src::Config,
			                              AffTrace::Call::Package0Inject,
			                              AffTrace::Mode::Queued);
		}
	}
	CHECK(AffTrace::ipcFrameCount() == base + 3, "every owner dispatcher frame counted");
	CHECK(AffTrace::ipcDepth() == 0, "depth back to zero");

	bool drainInFrame = false;
	bool callInFrame = false;
	for (const auto& l : readLines())
	{
		if (l.find("kind=QUEUE_DRAIN") != std::string::npos
		    && l.find("ipc_depth=1") != std::string::npos)
			drainInFrame = true;
		if (l.find("kind=STEAMCALL_EXIT") != std::string::npos
		    && l.find("mode=queued") != std::string::npos
		    && l.find("off_owner=0") != std::string::npos
		    && l.find("ipc_depth=1") != std::string::npos)
			callInFrame = true;
	}
	CHECK(drainInFrame, "the drain record reports ipc_depth >= 1 (was 0 before)");
	CHECK(callInFrame,
	      "queued Steam-owned work is recorded on the owner thread inside a frame");
}

static void test_hourly_growth()
{
	std::printf("[8] Q2 evidence: per-hour rollup counters\n");
	enableTracing();
	AffTrace::latchOwner();
	AffTrace::setRollupIntervalUs(1000);  // 1 ms buckets for the test

	for (int round = 0; round < 3; ++round)
	{
		AffTrace::ipcFrameEnter();
		for (int i = 0; i <= round; ++i)
		{
			std::thread watcher([]
			{
				auto watch = AffTrace::watchSpan(AffTrace::Src::Config);
				auto call = AffTrace::callSpan(AffTrace::Src::Config,
				                              AffTrace::Call::Package0Inject,
				                              AffTrace::Mode::Direct);
			});
			watcher.join();
		}
		AffTrace::ipcFrameExit();
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
		// Traffic in the new bucket flushes the previous bucket's counters.
		AffTrace::queuePush(AffTrace::Src::Config, AffTrace::Call::Package0Inject,
		                    AffTrace::Mode::Queued, 0);
	}

	std::vector<std::string> rollups;
	for (const auto& l : readLines())
		if (l.find("kind=HOUR_ROLLUP") != std::string::npos)
			rollups.push_back(l);

	CHECK(rollups.size() >= 2, "several per-hour rollup lines emitted");
	bool sawOverlaps = false;
	for (const auto& l : rollups)
		if (l.find("overlaps=0") == std::string::npos
		    && l.find("overlaps=") != std::string::npos)
			sawOverlaps = true;
	CHECK(sawOverlaps, "a rollup reports a non-zero overlap count");
	bool sawWatchCb = false;
	for (const auto& l : rollups)
		if (l.find("watch_cb=0 ") == std::string::npos
		    && l.find("watch_cb=") != std::string::npos)
			sawWatchCb = true;
	CHECK(sawWatchCb, "a rollup reports watcher-callback counts");
}

static void test_record_bound()
{
	std::printf("[9] record cap\n");
	enableTracing();
	for (std::size_t i = 0; i < AffTrace::kMaxRecords + 500; ++i)
	{
		AffTrace::queuePush(AffTrace::Src::Config, AffTrace::Call::Package0Inject,
		                    AffTrace::Mode::Queued, 1);
	}
	const auto lines = readLines();
	CHECK(lines.size() <= AffTrace::kMaxRecords + AffTrace::kMaxRollups,
	      "sink stops at the record cap (no unbounded growth)");
	CHECK(lines.size() >= AffTrace::kMaxRecords - 8, "cap is not hit prematurely");
}

int main()
{
	char tmpl[] = "/tmp/slssteam-afftrace-test-XXXXXX";
	const char* dir = ::mkdtemp(tmpl);
	if (dir == nullptr)
	{
		std::printf("FAIL: could not create a temp HOME\n");
		return 1;
	}
	g_dir = dir;

	test_opt_in_parsing();
	test_record_format_and_privacy();
	test_overlap_predicate();
	test_disabled_by_default();
	test_enabled_sink();
	test_offowner_overlap_capture();
	test_owner_frame_accounting();
	test_hourly_growth();
	test_record_bound();

	AffTrace::reset();
	::unlink(tracePath().c_str());
	::rmdir(g_dir.c_str());

	if (g_failures == 0)
	{
		std::printf("\ntest_afftrace: ALL PASS (%d checks)\n", g_checks);
		return 0;
	}
	std::printf("\ntest_afftrace: %d/%d CHECK(S) FAILED\n", g_failures, g_checks);
	return 1;
}
