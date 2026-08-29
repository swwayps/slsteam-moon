// SPDX-License-Identifier: AGPL-3.0-only
//
// Thread-affinity tracing — DIAGNOSTICS ONLY, DISABLED BY DEFAULT.
//
// WHAT IT ANSWERS
// ---------------
// A controlled VM run proved that watcher-originated Steam-owned calls execute
// off the latched owner IPC thread, but it could NOT answer two questions,
// because the client was idle and the tracing was temporary scaffolding that
// got reverted:
//
//   Q1: does a watcher-originated Steam-owned call ever run with off_owner=1
//       AND (ipc_depth >= 1 or frames_during >= 1)?  i.e. does the off-owner
//       call ever OVERLAP an in-flight owner IPC frame?
//   Q2: do those overlap counts grow per hour of session time?
//
// Shipping the instrumentation permanently (but off) means the next real user
// report can arrive with that evidence instead of needing a bespoke build.
// Nothing here asserts a cause for any client failure.
//
// ENABLING IT
// -----------
//   SLSSTEAM_AFFTRACE=1   in the environment of the Steam process
//
// Accepted true values: 1, y, yes, on, true (case-insensitive). Anything else
// (including unset) leaves tracing off. When off, every hook-side call is a
// single relaxed atomic load and an early return, so the IPC hot path is not
// touched in normal use.
//
// Output: $HOME/.SLSsteam.afftrace.log, created 0600, truncated at open,
// hard-capped at kMaxRecords event lines plus kMaxRollups per-hour summary
// lines. Once a cap is hit the sink stops writing (no rotation, no growth).
//
// PRIVACY
// -------
// Only these fields are ever written: sequence, monotonic microseconds, record
// kind, a fixed callback-source label, a fixed symbolic call label, a fixed
// execution-mode label, native TID, latched owner TID, off-owner flag, IPC
// frame depth, cumulative owner frame count, queue depth, durations, and
// frames-entered-during counts. No account id, app id, package id, depot id,
// payload, file path, token, raw pointer, save name, hostname, or command
// argument is emitted, and none is even passed in.
//
// Header-only so the pure parts (opt-in parsing, record formatting, gating,
// bounds) are host-unit-testable without a Steam process.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace AffTrace
{
	// Hard bounds on the sink. Deliberately small: this is an evidence
	// capture, not a log.
	inline constexpr std::size_t kMaxRecords = 4096;
	inline constexpr std::size_t kMaxRollups = 48;

	enum class Kind : std::uint8_t
	{
		IpcOwner = 0,     // one per process: the latched owner IPC TID
		IpcEnter,         // owner IPC frame entered
		IpcExit,          // owner IPC frame left
		WatchEnter,       // watcher callback entered
		WatchExit,        // watcher callback left
		SteamCallEnter,   // a unit of Steam-owned work started
		SteamCallExit,    // a unit of Steam-owned work finished
		SteamFnEnter,     // a single resolved Steam function entered
		SteamFnExit,      // a single resolved Steam function returned
		QueuePush,        // work handed to the owner-thread queue
		QueueDrain,       // owner thread took queued work
		QueueStale,       // bounded-staleness escape hatch ran queued work inline
		HourRollup,       // per-hour counters (answers Q2)
	};

	enum class Src : std::uint8_t
	{
		None = 0,
		Config,   // the SLSsteam config / stplug-in watcher pthread
		Api,      // the /tmp/SLSsteam.API watcher pthread
		Ipc,      // an owner IPC frame
		Site,     // the exact Steam call site
	};

	enum class Call : std::uint8_t
	{
		None = 0,
		OnModify,               // watcher callback
		Package0Inject,         // package-0 injection unit of work
		Package0Sync,           // complete desired-state package reconciliation
		LicenseReconcile,       // license broadcast unit of work
		InstallApp,             // app-manager install unit of work
		CompatMapping,          // live compatibility mapping unit of work
		CutlMemoryGrow,         // resolved Steam CUtlMemoryGrow
		MarkLicenseChanged,     // resolved CUser package-change marker
		ProcessLicenseUpdates,  // resolved CUser pending-update processor
		NotifyLicensesUpdated,  // Steam NotifyLicensesUpdated
		RunIpcFrame,            // IClientUtils::RunIPCFrame
		Drain,                  // owner-thread queue drain
	};

	enum class Mode : std::uint8_t
	{
		NA = 0,
		Queued,          // ran on the owner thread from the queue
		Direct,          // ran on the calling thread (owner thread itself)
		DirectNoOwner,   // ran directly: no owner TID latched yet
		DirectOverflow,  // ran directly: queue rejected the work
		DirectDisabled,  // ran directly: the handoff is switched off
		DirectStale,     // ran directly: owner never drained (escape hatch)
	};

	struct Record
	{
		std::uint64_t seq = 0;
		std::uint64_t tMonoUs = 0;
		Kind          kind = Kind::IpcOwner;
		Src           src = Src::None;
		Call          call = Call::None;
		Mode          mode = Mode::NA;
		long          tid = 0;
		long          ownerTid = 0;
		int           offOwner = 0;
		std::uint32_t ipcDepth = 0;
		std::uint64_t ipcFrames = 0;
		std::uint32_t queueDepth = 0;
		std::uint64_t durUs = 0;
		std::uint64_t framesDuring = 0;
	};

	// Per-hour summary line. This is the growth answer (Q2): one line per
	// elapsed session hour that saw any traffic.
	struct Rollup
	{
		std::uint64_t seq = 0;
		std::uint64_t tMonoUs = 0;
		std::uint64_t hour = 0;                // 0-based session hour index
		std::uint64_t watcherCallbacks = 0;
		std::uint64_t steamCalls = 0;
		std::uint64_t offOwnerSteamCalls = 0;
		std::uint64_t overlaps = 0;   // off_owner && (ipc_depth || frames_during)
		std::uint64_t ipcFrames = 0;  // cumulative owner frames at rollup time
	};

	// ---- pure helpers (unit-testable) -----------------------------------

	inline const char* kindName(Kind k)
	{
		switch (k)
		{
			case Kind::IpcOwner:       return "IPC_OWNER";
			case Kind::IpcEnter:       return "IPC_ENTER";
			case Kind::IpcExit:        return "IPC_EXIT";
			case Kind::WatchEnter:     return "WATCH_ENTER";
			case Kind::WatchExit:      return "WATCH_EXIT";
			case Kind::SteamCallEnter: return "STEAMCALL_ENTER";
			case Kind::SteamCallExit:  return "STEAMCALL_EXIT";
			case Kind::SteamFnEnter:   return "STEAMFN_ENTER";
			case Kind::SteamFnExit:    return "STEAMFN_EXIT";
			case Kind::QueuePush:      return "QUEUE_PUSH";
			case Kind::QueueDrain:     return "QUEUE_DRAIN";
			case Kind::QueueStale:     return "QUEUE_STALE";
			case Kind::HourRollup:     return "HOUR_ROLLUP";
		}
		return "UNKNOWN";
	}

	inline const char* srcName(Src s)
	{
		switch (s)
		{
			case Src::None:   return "none";
			case Src::Config: return "config";
			case Src::Api:    return "api";
			case Src::Ipc:    return "ipc";
			case Src::Site:   return "site";
		}
		return "none";
	}

	inline const char* callName(Call c)
	{
		switch (c)
		{
			case Call::None:                  return "none";
			case Call::OnModify:              return "on_modify";
			case Call::Package0Inject:        return "package0_inject";
			case Call::Package0Sync:          return "package0_sync";
			case Call::LicenseReconcile:      return "license_reconcile";
			case Call::InstallApp:            return "install_app";
			case Call::CompatMapping:         return "compat_mapping";
			case Call::CutlMemoryGrow:        return "cutlmemory_grow";
			case Call::MarkLicenseChanged:    return "mark_license_changed";
			case Call::ProcessLicenseUpdates: return "process_license_updates";
			case Call::NotifyLicensesUpdated: return "notify_licenses_updated";
			case Call::RunIpcFrame:           return "run_ipc_frame";
			case Call::Drain:                 return "drain";
		}
		return "none";
	}

	inline const char* modeName(Mode m)
	{
		switch (m)
		{
			case Mode::NA:             return "n/a";
			case Mode::Queued:         return "queued";
			case Mode::Direct:         return "direct";
			case Mode::DirectNoOwner:  return "direct_no_owner";
			case Mode::DirectOverflow: return "direct_overflow";
			case Mode::DirectDisabled: return "direct_disabled";
			case Mode::DirectStale:    return "direct_stale";
		}
		return "n/a";
	}

	// Parse the opt-in value. Unset / empty / anything unrecognised -> off.
	inline bool optInEnabled(const char* value)
	{
		if (value == nullptr || value[0] == '\0')
			return false;
		std::string v(value);
		for (char& c : v)
			c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
		return v == "1" || v == "y" || v == "yes" || v == "on" || v == "true";
	}

	// One trace line. Fixed field order, all numeric or symbolic.
	inline std::string format(const Record& r)
	{
		char buf[352];
		std::snprintf(buf, sizeof(buf),
			"seq=%llu t_mono_us=%llu kind=%s src=%s call=%s mode=%s tid=%ld "
			"owner_tid=%ld off_owner=%d ipc_depth=%u ipc_frames=%llu "
			"queue_depth=%u dur_us=%llu frames_during=%llu\n",
			static_cast<unsigned long long>(r.seq),
			static_cast<unsigned long long>(r.tMonoUs),
			kindName(r.kind), srcName(r.src), callName(r.call), modeName(r.mode),
			r.tid, r.ownerTid, r.offOwner,
			r.ipcDepth,
			static_cast<unsigned long long>(r.ipcFrames),
			r.queueDepth,
			static_cast<unsigned long long>(r.durUs),
			static_cast<unsigned long long>(r.framesDuring));
		return std::string(buf);
	}

	// One per-hour summary line.
	inline std::string formatRollup(const Rollup& s)
	{
		char buf[288];
		std::snprintf(buf, sizeof(buf),
			"seq=%llu t_mono_us=%llu kind=%s hour=%llu watch_cb=%llu "
			"steam_calls=%llu off_owner_calls=%llu overlaps=%llu ipc_frames=%llu\n",
			static_cast<unsigned long long>(s.seq),
			static_cast<unsigned long long>(s.tMonoUs),
			kindName(Kind::HourRollup),
			static_cast<unsigned long long>(s.hour),
			static_cast<unsigned long long>(s.watcherCallbacks),
			static_cast<unsigned long long>(s.steamCalls),
			static_cast<unsigned long long>(s.offOwnerSteamCalls),
			static_cast<unsigned long long>(s.overlaps),
			static_cast<unsigned long long>(s.ipcFrames));
		return std::string(buf);
	}

	// True when this record proves an off-owner Steam-owned call overlapping
	// owner IPC work — the exact Q1 predicate.
	inline bool isOverlap(const Record& r)
	{
		return r.kind == Kind::SteamCallExit && r.offOwner != 0
		    && (r.ipcDepth >= 1 || r.framesDuring >= 1);
	}

	// ---- process state --------------------------------------------------

	namespace detail
	{
		inline std::atomic<bool>          g_enabled{ false };
		inline std::atomic<bool>          g_initDone{ false };
		inline std::atomic<long>          g_ownerTid{ 0 };
		inline std::atomic<std::uint64_t> g_seq{ 0 };
		inline std::atomic<std::uint32_t> g_ipcDepth{ 0 };
		inline std::atomic<std::uint64_t> g_ipcFrames{ 0 };
		// Non-zero while a watcher-originated Steam-owned call is in flight.
		// Per-frame IPC records are gated on this so the hot path stays quiet.
		inline std::atomic<int>           g_callWindows{ 0 };
		inline std::atomic<std::uint64_t> g_records{ 0 };
		inline std::atomic<std::uint64_t> g_rollups{ 0 };
		inline std::atomic<std::uint64_t> g_startUs{ 0 };
		inline std::atomic<std::uint64_t> g_hourBucket{ 0 };
		// Rollup bucket width. One hour in the client; shrunk by the unit test
		// so the per-hour growth behaviour is verifiable without waiting.
		inline std::atomic<std::uint64_t> g_rollupUs{ 3600ULL * 1000000ULL };

		inline std::mutex& sinkMutex()
		{
			static std::mutex m;
			return m;
		}
		inline int& sinkFd()
		{
			static int fd = -1;
			return fd;
		}

		// Counters for the current hour bucket, plus the cumulative set.
		inline std::atomic<std::uint64_t> g_hourWatchCb{ 0 };
		inline std::atomic<std::uint64_t> g_hourSteamCalls{ 0 };
		inline std::atomic<std::uint64_t> g_hourOffOwner{ 0 };
		inline std::atomic<std::uint64_t> g_hourOverlaps{ 0 };
	}

	inline bool enabled()
	{
		return detail::g_enabled.load(std::memory_order_relaxed);
	}

	inline std::uint64_t monotonicUs()
	{
		struct timespec ts{};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL
		     + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
	}

	inline long selfTid()
	{
		// Cached per thread: frame accounting now sits on EVERY hooked
		// dispatcher, so this must not cost a syscall per IPC frame.
		static thread_local long tid = 0;
		if (tid == 0)
			tid = static_cast<long>(syscall(SYS_gettid));
		return tid;
	}

	inline long ownerTid()
	{
		return detail::g_ownerTid.load(std::memory_order_relaxed);
	}

	// True when the calling thread is the latched owner IPC thread. Frame
	// accounting is gated on this so `ipc_depth` / `ipc_frames` keep meaning
	// "owner-thread frames" even though the guard is now installed on every
	// hooked dispatcher.
	inline bool onOwnerThread()
	{
		const long owner = detail::g_ownerTid.load(std::memory_order_relaxed);
		return owner != 0 && owner == selfTid();
	}

	inline std::uint64_t ipcFrameCount()
	{
		return detail::g_ipcFrames.load(std::memory_order_relaxed);
	}

	inline std::uint32_t ipcDepth()
	{
		return detail::g_ipcDepth.load(std::memory_order_relaxed);
	}

	// Write one already-bounded line. Caller must have checked enabled().
	inline void writeLine(const std::string& line)
	{
		std::lock_guard<std::mutex> lk(detail::sinkMutex());
		const int fd = detail::sinkFd();
		if (fd < 0)
			return;
		const ssize_t written = ::write(fd, line.data(), line.size());
		(void)written;  // a failed trace write must never affect the client
	}

	// Emit the previous hour's counters when the hour bucket advances. Lazy:
	// driven by traffic, so an hour with no records produces no line.
	inline void maybeRollup(std::uint64_t nowUs)
	{
		const std::uint64_t start = detail::g_startUs.load(std::memory_order_relaxed);
		if (start == 0 || nowUs < start)
			return;
		const std::uint64_t width = detail::g_rollupUs.load(std::memory_order_relaxed);
		const std::uint64_t hour = (nowUs - start) / (width ? width : 1);
		std::uint64_t seen = detail::g_hourBucket.load(std::memory_order_relaxed);
		if (hour == seen)
			return;
		if (!detail::g_hourBucket.compare_exchange_strong(seen, hour))
			return;
		if (detail::g_rollups.fetch_add(1) >= kMaxRollups)
			return;

		Rollup s;
		s.seq = detail::g_seq.fetch_add(1);
		s.tMonoUs = nowUs;
		s.hour = seen;  // the hour index the counters belong to
		s.watcherCallbacks = detail::g_hourWatchCb.exchange(0);
		s.steamCalls = detail::g_hourSteamCalls.exchange(0);
		s.offOwnerSteamCalls = detail::g_hourOffOwner.exchange(0);
		s.overlaps = detail::g_hourOverlaps.exchange(0);
		s.ipcFrames = detail::g_ipcFrames.load(std::memory_order_relaxed);
		writeLine(formatRollup(s));
	}

	inline void emit(Record& r)
	{
		if (!enabled())
			return;
		if (detail::g_records.fetch_add(1) >= kMaxRecords)
			return;
		r.seq = detail::g_seq.fetch_add(1);
		if (r.tMonoUs == 0)
			r.tMonoUs = monotonicUs();
		maybeRollup(r.tMonoUs);
		if (r.tid == 0)
			r.tid = selfTid();
		r.ownerTid = ownerTid();
		r.offOwner = (r.ownerTid != 0 && r.tid != r.ownerTid) ? 1 : 0;
		writeLine(format(r));
	}

	// Open the sink. Public for tests; init() is the normal entry point.
	inline bool openSink(const std::string& path)
	{
		std::lock_guard<std::mutex> lk(detail::sinkMutex());
		if (detail::sinkFd() >= 0)
			return true;
		const int fd = ::open(path.c_str(),
			O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd < 0)
			return false;
		// Tighten an inherited mode if the file already existed.
		(void)::fchmod(fd, 0600);
		detail::sinkFd() = fd;
		return true;
	}

	inline std::string defaultPath()
	{
		const char* home = std::getenv("HOME");
		if (home == nullptr || home[0] == '\0')
			return std::string();
		return std::string(home) + "/.SLSsteam.afftrace.log";
	}

	// Read the opt-in and, when set, open the bounded 0600 sink. Cheap and
	// side-effect-free when the opt-in is absent (the normal case).
	inline bool init()
	{
		bool expected = false;
		if (!detail::g_initDone.compare_exchange_strong(expected, true))
			return enabled();
		if (!optInEnabled(std::getenv("SLSSTEAM_AFFTRACE")))
			return false;

		const std::string path = defaultPath();
		if (path.empty() || !openSink(path))
			return false;

		detail::g_startUs.store(monotonicUs(), std::memory_order_relaxed);
		detail::g_enabled.store(true, std::memory_order_relaxed);
		return true;
	}

	// Test-only: close the sink and forget all state so a test can re-init
	// with different settings. Never called from the client.
	inline void reset()
	{
		{
			std::lock_guard<std::mutex> lk(detail::sinkMutex());
			if (detail::sinkFd() >= 0)
			{
				::close(detail::sinkFd());
				detail::sinkFd() = -1;
			}
		}
		detail::g_enabled.store(false);
		detail::g_initDone.store(false);
		detail::g_ownerTid.store(0);
		detail::g_seq.store(0);
		detail::g_ipcDepth.store(0);
		detail::g_ipcFrames.store(0);
		detail::g_callWindows.store(0);
		detail::g_records.store(0);
		detail::g_rollups.store(0);
		detail::g_startUs.store(0);
		detail::g_hourBucket.store(0);
		detail::g_hourWatchCb.store(0);
		detail::g_hourSteamCalls.store(0);
		detail::g_hourOffOwner.store(0);
		detail::g_hourOverlaps.store(0);
		detail::g_rollupUs.store(3600ULL * 1000000ULL);
	}

	// Test-only: shrink the per-hour rollup bucket so the growth behaviour can
	// be exercised without a multi-hour run. Never called from the client.
	inline void setRollupIntervalUs(std::uint64_t us)
	{
		detail::g_rollupUs.store(us ? us : 1);
	}

	// ---- call-site helpers ---------------------------------------------

	// Latch the owner IPC thread once per process (from the hooked
	// IClientUtils::RunIPCFrame, which is the dispatcher the VM run measured).
	inline void latchOwner()
	{
		if (!enabled())
			return;
		long expected = 0;
		const long self = selfTid();
		if (!detail::g_ownerTid.compare_exchange_strong(expected, self))
			return;
		Record r;
		r.kind = Kind::IpcOwner;
		r.src = Src::Ipc;
		r.call = Call::RunIpcFrame;
		r.tid = self;
		emit(r);
	}

	// Owner IPC frame accounting. Records are written only while a
	// watcher-originated Steam-owned call is in flight; the counters
	// themselves are two relaxed atomics.
	inline void ipcFrameEnter()
	{
		if (!enabled())
			return;
		detail::g_ipcFrames.fetch_add(1, std::memory_order_relaxed);
		const std::uint32_t depth =
			detail::g_ipcDepth.fetch_add(1, std::memory_order_relaxed) + 1;
		if (detail::g_callWindows.load(std::memory_order_relaxed) == 0)
			return;
		Record r;
		r.kind = Kind::IpcEnter;
		r.src = Src::Ipc;
		r.call = Call::RunIpcFrame;
		r.ipcDepth = depth;
		r.ipcFrames = detail::g_ipcFrames.load(std::memory_order_relaxed);
		emit(r);
	}

	inline void ipcFrameExit()
	{
		if (!enabled())
			return;
		const std::uint32_t before =
			detail::g_ipcDepth.fetch_sub(1, std::memory_order_relaxed);
		if (detail::g_callWindows.load(std::memory_order_relaxed) == 0)
			return;
		Record r;
		r.kind = Kind::IpcExit;
		r.src = Src::Ipc;
		r.call = Call::RunIpcFrame;
		r.ipcDepth = before ? before - 1 : 0;
		r.ipcFrames = detail::g_ipcFrames.load(std::memory_order_relaxed);
		emit(r);
	}

	inline void queuePush(Src src, Call call, Mode mode, std::uint32_t queueDepth)
	{
		if (!enabled())
			return;
		Record r;
		r.kind = Kind::QueuePush;
		r.src = src;
		r.call = call;
		r.mode = mode;
		r.queueDepth = queueDepth;
		r.ipcDepth = ipcDepth();
		r.ipcFrames = ipcFrameCount();
		emit(r);
	}

	inline void queueDrain(std::uint64_t taken, std::uint32_t queueDepth)
	{
		if (!enabled())
			return;
		Record r;
		r.kind = Kind::QueueDrain;
		r.src = Src::Ipc;
		r.call = Call::Drain;
		r.mode = Mode::Queued;
		r.queueDepth = queueDepth;
		r.framesDuring = taken;
		r.ipcDepth = ipcDepth();
		r.ipcFrames = ipcFrameCount();
		emit(r);
	}

	// The bounded-staleness escape hatch fired: the owner never drained, so
	// queued work ran on the calling (non-owner) thread instead. `src` is left
	// unset because the flushing thread is not necessarily the thread that
	// enqueued the work.
	inline void queueStale(std::uint64_t flushed, std::uint32_t queueDepth)
	{
		if (!enabled())
			return;
		Record r;
		r.kind = Kind::QueueStale;
		r.src = Src::None;
		r.call = Call::Drain;
		r.mode = Mode::DirectStale;
		r.queueDepth = queueDepth;
		r.framesDuring = flushed;
		r.ipcDepth = ipcDepth();
		r.ipcFrames = ipcFrameCount();
		emit(r);
	}

	// RAII owner-IPC-frame accounting, so the depth counter cannot drift if a
	// frame leaves through an unexpected path. Inert when tracing is off.
	//
	// Installed on EVERY hooked RunIPCFrame dispatcher, not just IClientUtils.
	// The first VM run only guarded IClientUtils, so `ipc_depth`/`ipc_frames`
	// — and therefore HOUR_ROLLUP.overlaps — under-reported real owner-frame
	// activity: the successful drain came from another dispatcher and was
	// recorded with ipc_depth=0. Counting is gated on actually being the
	// latched owner thread, so a dispatcher that runs elsewhere still cannot
	// pollute the owner-frame counters. The enter/exit decision is taken once,
	// in the constructor, so the pair can never become unbalanced if the owner
	// is latched mid-frame.
	class FrameGuard
	{
	public:
		FrameGuard() : m_counting(enabled() && onOwnerThread())
		{
			if (m_counting)
				ipcFrameEnter();
		}
		~FrameGuard()
		{
			if (m_counting)
				ipcFrameExit();
		}
		FrameGuard(const FrameGuard&) = delete;
		FrameGuard& operator=(const FrameGuard&) = delete;

		bool counting() const { return m_counting; }

	private:
		bool m_counting;
	};

	// RAII span. Emits an enter record on construction and an exit record
	// carrying dur_us and frames_during on destruction. Entirely inert when
	// tracing is off (one relaxed load in the constructor).
	class Scope
	{
	public:
		Scope(Kind enterKind, Kind exitKind, Src src, Call call, Mode mode,
		      bool openCallWindow = false)
			: m_exitKind(exitKind), m_src(src), m_call(call), m_mode(mode),
			  m_window(openCallWindow), m_active(enabled())
		{
			if (!m_active)
				return;
			if (m_window)
				detail::g_callWindows.fetch_add(1, std::memory_order_relaxed);
			m_startUs = monotonicUs();
			m_startFrames = ipcFrameCount();
			Record r;
			r.tMonoUs = m_startUs;
			r.kind = enterKind;
			r.src = m_src;
			r.call = m_call;
			r.mode = m_mode;
			r.ipcDepth = ipcDepth();
			r.ipcFrames = m_startFrames;
			emit(r);
		}

		~Scope()
		{
			if (!m_active)
				return;
			const std::uint64_t now = monotonicUs();
			Record r;
			r.tMonoUs = now;
			r.kind = m_exitKind;
			r.src = m_src;
			r.call = m_call;
			r.mode = m_mode;
			r.ipcDepth = ipcDepth();
			r.ipcFrames = ipcFrameCount();
			r.durUs = now - m_startUs;
			r.framesDuring = r.ipcFrames - m_startFrames;
			r.tid = selfTid();
			r.ownerTid = ownerTid();
			r.offOwner = (r.ownerTid != 0 && r.tid != r.ownerTid) ? 1 : 0;

			if (m_exitKind == Kind::WatchExit)
				detail::g_hourWatchCb.fetch_add(1, std::memory_order_relaxed);
			if (m_exitKind == Kind::SteamCallExit)
			{
				detail::g_hourSteamCalls.fetch_add(1, std::memory_order_relaxed);
				if (r.offOwner)
					detail::g_hourOffOwner.fetch_add(1, std::memory_order_relaxed);
				if (isOverlap(r))
					detail::g_hourOverlaps.fetch_add(1, std::memory_order_relaxed);
			}

			emit(r);
			if (m_window)
				detail::g_callWindows.fetch_sub(1, std::memory_order_relaxed);
		}

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Kind          m_exitKind;
		Src           m_src;
		Call          m_call;
		Mode          m_mode;
		bool          m_window;
		bool          m_active;
		std::uint64_t m_startUs = 0;
		std::uint64_t m_startFrames = 0;
	};

	// Convenience spans for the three levels the VM run used.
	inline Scope watchSpan(Src src)
	{
		return Scope(Kind::WatchEnter, Kind::WatchExit, src, Call::OnModify, Mode::NA);
	}

	// A unit of Steam-owned work. Opens the call window so owner IPC frames
	// are recorded for its duration (that is the overlap evidence).
	inline Scope callSpan(Src src, Call call, Mode mode)
	{
		return Scope(Kind::SteamCallEnter, Kind::SteamCallExit, src, call, mode, true);
	}

	// A single resolved Steam function.
	inline Scope fnSpan(Call call, Mode mode)
	{
		return Scope(Kind::SteamFnEnter, Kind::SteamFnExit, Src::Site, call, mode);
	}
}
