// Hooks::setup() must fail, without patching anything, when a required hook
// has no target or its target cannot be patched; load() then leaves Steam
// unmodified instead of reporting success with the hook missing. TraceIPC
// only logs, so it is not required.
// Build: make test-hook-placement (links the whole object set).
#include "../src/config.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/feats/manifestbind.hpp"
#include "../src/hooks.hpp"
#include "../src/log.hpp"
#include "../src/patterns.hpp"
#include "../src/sdk/CNetPacket.hpp"
#include "../src/sdk/CUtl.hpp"
#include "../src/sdk/protobufs/steammessages_base.pb.h"
#include "../src/vftableinfo.hpp"
#include "fake_libcurl.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

// Once armed, the next allocation on this thread fails.
static thread_local bool t_failNextAllocation = false;
static thread_local bool t_allocationFailed = false;
// Counts down allocations on this thread; the one that reaches zero fails.
static thread_local int t_failIn = 0;
void* operator new(std::size_t size)
{
	if (t_failIn > 0 && --t_failIn == 0)
		t_failNextAllocation = true;
	if (t_failNextAllocation)
	{
		t_failNextAllocation = false;
		t_allocationFailed = true;
		throw std::bad_alloc();
	}
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace
{

// True when `scenario` returns true in a child process; a crash counts as false.
template <typename Scenario>
bool inChild(Scenario scenario)
{
	std::fflush(stdout);
	const pid_t pid = fork();
	if (pid == 0) std::_Exit(scenario() ? 0 : 1);
	int status = 0;
	return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0;
}

void pointRequiredHooksAt(lm_address_t target, lm_address_t stride = 0)
{
	for (Pattern_t* pattern : {&Patterns::CWebSocketConnection::BBuildAndAsyncSendFrame,
	                           &Patterns::CJobMgr::BRouteMsgToJob,
	                           &Patterns::CDepotDownloadMgr::BYldRequestDepotManifest,
	                           &Patterns::CUser::CheckAppOwnership,
	                           &Patterns::CUser::GetSubscribedApps,
	                           &Patterns::CUser::PostCallbackToAppId,
	                           &Patterns::CSteamEngine::ProcessIPCFrame,
	                           &Patterns::CSteamEngine::SetAppIdForCurrentPipe})
	{
		pattern->address = target;
		target += stride;
	}
	VFTIndexes::CCMInterface::RecvPkt.address = target;
}

class NotFoundCurl final : public FakeCurl
{
public:
	CURL* init() override { return reinterpret_cast<CURL*>(this); }
	CURLcode setopt(CURL*, CURLoption, long, void*) override { return CURLE_OK; }
	CURLcode perform(CURL*) override { return CURLE_OK; }
	void cleanup(CURL*) override {}
	CURLcode getinfo(CURL*, CURLINFO info, void* out) override
	{
		if (info == CURLINFO_RESPONSE_CODE) *static_cast<long*>(out) = 404;
		return CURLE_OK;
	}
	CURLcode send(CURL*, const void*, std::size_t, std::size_t*) override
	{
		return CURLE_UNSUPPORTED_PROTOCOL;
	}
	CURLcode recv(CURL*, void*, std::size_t, std::size_t*) override
	{
		return CURLE_UNSUPPORTED_PROTOCOL;
	}
};

using DepotFn = void* (*)(void*, void*, uint32_t, uint32_t, uint64_t, void*);
static uint64_t g_plannerReceivedGid = 0;
static uint64_t g_leafReceivedGid = 0;
static volatile DepotFn g_nestedLeafTarget = nullptr;

__attribute__((noinline)) void* stubProcessDepot(
	void* ctx, void* a0C, uint32_t appId, uint32_t depotId, uint64_t manifestId, void* a20)
{
	asm volatile(
		"nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
		"nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t" ::: "memory");
	(void)ctx; (void)a0C; (void)appId; (void)depotId; (void)a20;
	g_leafReceivedGid = manifestId;
	return nullptr;
}

__attribute__((noinline)) void* stubPrepareDepot(
	void* ctx, void* a0C, uint32_t appId, uint32_t depotId, uint64_t manifestId, void* a20)
{
	asm volatile(
		"nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
		"nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t" ::: "memory");
	const bool hadFailNext = t_failNextAllocation;
	const int remainingFailIn = t_failIn;
	g_plannerReceivedGid = manifestId;
	const DepotFn leaf = g_nestedLeafTarget;
	void* out = leaf ? leaf(ctx, a0C, appId, depotId, manifestId, a20) : nullptr;
	(void)hadFailNext; (void)remainingFailIn;
	return out;
}

void writeValidManifest(const std::filesystem::path& path)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	const uint32_t magic = 0x71F617D0u;
	const uint32_t zeros[2] = {0, 0};
	const uint32_t end = 0x32C415ABu;
	out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
	out.write(reinterpret_cast<const char*>(zeros), sizeof(zeros));
	out.write(reinterpret_cast<const char*>(&end), sizeof(end));
}

} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_hook_placement_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	const std::string logPath = std::string(home) + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	CHECK(inChild([] { return !Hooks::setup(); }),
	      "with no target resolved, Hooks::setup() fails without placing hooks");

	// An executable page whose bytes do not decode, so no detour can patch it.
	void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) return 2;
	std::memset(page, 0xFF, 4096);
	if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) return 2;
	CHECK(inChild([page] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(page));
		      const bool failed = !Hooks::setup();
		      const auto* bytes = static_cast<const unsigned char*>(page);
		      return failed && bytes[0] == 0xFF && bytes[4] == 0xFF;
	      }),
	      "a required hook that cannot be patched fails Hooks::setup()");

	// Small functions every detour can patch and call on x86-64 (14-byte jump).
	void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (code == MAP_FAILED) return 2;
	const unsigned char stub[] = {
		0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x10,
		0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x31, 0xC0, 0xC9, 0xC3
	};
	for (std::size_t at = 0; at + sizeof(stub) <= 4096; at += 64)
		std::memcpy(static_cast<unsigned char*>(code) + at, stub, sizeof(stub));
	if (mprotect(code, 4096, PROT_READ | PROT_EXEC) != 0) return 2;
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      return Hooks::setup();
	      }),
	      "an unresolved TraceIPC does not fail Hooks::setup()");

	// The CM receive hook carries the Family Share choke and every CM message
	// feature, so it is required like the others.
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      VFTIndexes::CCMInterface::RecvPkt.address = 0;
		      return !Hooks::setup();
	      }),
	      "an unresolved CM receive hook fails Hooks::setup()");
	CHECK(inChild([code, page] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      VFTIndexes::CCMInterface::RecvPkt.address = reinterpret_cast<lm_address_t>(page);
		      return !Hooks::setup();
	      }),
	      "a CM receive hook that cannot be patched fails Hooks::setup()");
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      const auto* bytes = static_cast<const unsigned char*>(code);
		      const std::vector<unsigned char> before(bytes, bytes + 4096);
		      if (!Hooks::setup()) return false;
		      const bool patched = std::memcmp(bytes, before.data(), 4096) != 0;
		      Hooks::remove();
		      return patched && std::memcmp(bytes, before.data(), 4096) == 0;
	      }),
	      "Hooks::remove() restores every hook Hooks::setup() placed");

	// The CM receive hook's own work before the dispatch boundary (packet
	// layout, logging, the Family Share decision) can run out of memory on
	// Steam's receive thread: the hook still hands the packet to Steam.
	CHECK(inChild([code] {
		      pointRequiredHooksAt(reinterpret_cast<lm_address_t>(code), 64);
		      if (!Hooks::setup()) return false;
		      g_config.disableFamilyLock.set(true);
		      CMsgProtoBufHeader header;
		      header.set_target_job_name("FamilyGroupsClient.NotifyRunningApps#1");
		      const std::string bytes = header.SerializeAsString();
		      std::vector<unsigned char> storage(sizeof(CNetPacketBody) + bytes.size());
		      auto* body = reinterpret_cast<CNetPacketBody*>(storage.data());
		      body->type = CNetPacket::PROTOBUF_TYPE_MASK | 146;
		      body->headerSize = bytes.size();
		      std::memcpy(storage.data() + sizeof(CNetPacketBody), bytes.data(), bytes.size());
		      CNetPacket packet{};
		      packet.setBody(body);
		      packet.setSize(storage.size());
		      using RecvPkt = void (*)(void*, CNetPacket*);
		      const auto recv = reinterpret_cast<RecvPkt>(
		          VFTIndexes::CCMInterface::RecvPkt.address);
		      t_failNextAllocation = true;
		      recv(nullptr, &packet);
		      t_failNextAllocation = false;
		      return t_allocationFailed;
	      }),
	      "an allocation failure before the CM dispatch does not unwind into Steam");

	// The download-plan hooks run our work on Steam's planning thread. An
	// allocation that fails anywhere in it must not unwind into Steam, and
	// the depot list Steam plans from must stay whole: either filtered as a
	// whole (the managed size-0 depot dropped) or left as it came.
	int planRounds = 0;
	bool planSafe = true;
	for (int k = 1; k <= 300; ++k)
	{
		int pipeFds[2];
		if (pipe(pipeFds) != 0) return 2;
		std::fflush(stdout);
		const pid_t pid = fork();
		if (pid == 0)
		{
			close(pipeFds[0]);
			const auto base = reinterpret_cast<lm_address_t>(code);
			Patterns::CDepotDownloadMgr::ProcessDepotManifest.address = base + 10 * 64;
			Patterns::CDepotDownloadMgr::PrepareDepotDownload.address = base + 11 * 64;
			Patterns::CDepotDownloadMgr::BuildDepotDependency.address = base + 12 * 64;
			if (!ManifestBind::setup()) std::_Exit(3);
			(void)DepotKey::saveKeyToCache(42, 4202, std::string(32, 'k'), true);
			alignas(8) unsigned char entries[4 * 0x20] = {};
			const uint32_t depots[4] = {4201, 4202, 4203, 4204};
			const uint64_t sizes[4] = {100, 0, 100, 100};
			for (int i = 0; i < 4; ++i)
			{
				const uint32_t app = 42;
				const uint64_t gid = 7;
				std::memcpy(entries + i * 0x20, &depots[i], 4);
				std::memcpy(entries + i * 0x20 + 0x04, &app, 4);
				std::memcpy(entries + i * 0x20 + 0x08, &gid, 8);
				std::memcpy(entries + i * 0x20 + 0x10, &sizes[i], 8);
			}
			alignas(8) unsigned char vec[sizeof(CUtlVector<char>)] = {};
			void* entryBase = entries;
			const int32_t capacity = 4;
			int32_t count = 4;
			std::memcpy(vec + offsetof(CUtlMemory<char>, base), &entryBase, sizeof(entryBase));
			std::memcpy(vec + offsetof(CUtlMemory<char>, alloc), &capacity, 4);
			std::memcpy(vec + offsetof(CUtlVector<char>, size), &count, 4);
			using BuildFn = void* (*)(void*, uint32_t, void*, void*);
			t_allocationFailed = false;
			t_failIn = k;
			reinterpret_cast<BuildFn>(base + 12 * 64)(nullptr, 0, vec, nullptr);
			reinterpret_cast<DepotFn>(base + 10 * 64)(nullptr, nullptr, 42, 4201, 7, nullptr);
			reinterpret_cast<DepotFn>(base + 11 * 64)(nullptr, nullptr, 42, 4201, 7, nullptr);
			t_failIn = 0;
			t_failNextAllocation = false;
			std::memcpy(&count, vec + offsetof(CUtlVector<char>, size), 4);
			uint32_t seen[4] = {};
			for (int i = 0; i < 4; ++i) std::memcpy(&seen[i], entries + i * 0x20, 4);
			const bool whole =
				(count == 3 && seen[0] == 4201 && seen[1] == 4203 && seen[2] == 4204)
				|| (count == 4 && seen[0] == 4201 && seen[1] == 4202 && seen[2] == 4203
				    && seen[3] == 4204);
			const char result[2] = {static_cast<char>(t_allocationFailed), static_cast<char>(whole)};
			(void)!write(pipeFds[1], result, 2);
			std::_Exit(0);
		}
		close(pipeFds[1]);
		char result[2] = {0, 0};
		const bool reported = read(pipeFds[0], result, 2) == 2;
		close(pipeFds[0]);
		int status = 0;
		waitpid(pid, &status, 0);
		if (!reported || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			planSafe = false;
			std::printf("allocation %d: the plan hooks did not return\n", k);
			break;
		}
		if (!result[0]) break; // the calls no longer reach their k-th allocation
		++planRounds;
		if (!result[1])
		{
			planSafe = false;
			std::printf("allocation %d: the depot list was left half-filtered\n", k);
			break;
		}
	}
	CHECK(planRounds > 0 && planSafe,
	      "an allocation failure in the plan hooks does not reach Steam or split the depot list");

	// R3-M-1: PrepareDepotDownload calls ProcessDepotManifest with the gid it
	// chose. The nested leaf call must keep that gid (leaving offline_<app> in
	// place on a local fallback, and never diverging after a planner failure).
	static NotFoundCurl notFoundCurl;
	g_fakeCurl = &notFoundCurl;
	CHECK(inChild([code, &home] {
		      const std::filesystem::path steamRoot = std::filesystem::path(home) / ".steam" / "steam";
		      std::filesystem::create_directories(steamRoot / "depotcache");
		      std::ofstream(steamRoot / "steam.sh") << "#!/bin/sh\n";
		      const std::filesystem::path store =
		          std::filesystem::path(home) / ".config" / "SLSsteam" / "manifests";
		      writeValidManifest(store / "4201_8.manifest");
		      std::ofstream(store / ".preferred_4201") << "8\n";
		      g_config.addedAppIds.set({42});
		      g_config.managedAppIds.set({42});
		      (void)DepotKey::saveKeyToCache(42, 4201, std::string(32, 'k'), true);

		      const auto base = reinterpret_cast<lm_address_t>(code);
		      Patterns::CDepotDownloadMgr::ProcessDepotManifest.address =
		          reinterpret_cast<lm_address_t>(&stubProcessDepot);
		      Patterns::CDepotDownloadMgr::PrepareDepotDownload.address =
		          reinterpret_cast<lm_address_t>(&stubPrepareDepot);
		      Patterns::CDepotDownloadMgr::BuildDepotDependency.address = base + 12 * 64;
		      g_nestedLeafTarget = &stubProcessDepot;
		      if (!ManifestBind::setup()) return false;

		      stubPrepareDepot(nullptr, nullptr, 42, 4201, 7, nullptr);
		      return g_plannerReceivedGid == 8 && g_leafReceivedGid == 8 &&
		             std::filesystem::exists(std::filesystem::path(home) / "SLSsteam" / "offline_42");
	      }),
	      "a fallback plan keeps offline_<app> across PrepareDepotDownload's nested leaf call");

	int nestedRounds = 0;
	bool nestedConsistent = true;
	for (int k = 1; k <= 300; ++k)
	{
		int pipeFds[2];
		if (pipe(pipeFds) != 0) return 2;
		std::fflush(stdout);
		const pid_t pid = fork();
		if (pid == 0)
		{
			close(pipeFds[0]);
			const std::filesystem::path steamRoot = std::filesystem::path(home) / ".steam" / "steam";
			std::filesystem::remove_all(steamRoot / "depotcache");
			std::filesystem::create_directories(steamRoot / "depotcache");
			std::ofstream(steamRoot / "steam.sh") << "#!/bin/sh\n";
			const std::filesystem::path store =
			    std::filesystem::path(home) / ".config" / "SLSsteam" / "manifests";
			writeValidManifest(store / "4201_8.manifest");
			std::ofstream(store / ".preferred_4201") << "8\n";
			g_config.addedAppIds.set({42});
			g_config.managedAppIds.set({42});
			(void)DepotKey::saveKeyToCache(42, 4201, std::string(32, 'k'), true);

			const auto base = reinterpret_cast<lm_address_t>(code);
			Patterns::CDepotDownloadMgr::ProcessDepotManifest.address =
			    reinterpret_cast<lm_address_t>(&stubProcessDepot);
			Patterns::CDepotDownloadMgr::PrepareDepotDownload.address =
			    reinterpret_cast<lm_address_t>(&stubPrepareDepot);
			Patterns::CDepotDownloadMgr::BuildDepotDependency.address = base + 12 * 64;
			g_nestedLeafTarget = &stubProcessDepot;
			if (!ManifestBind::setup()) std::_Exit(3);

			g_plannerReceivedGid = 0;
			g_leafReceivedGid = 0;
			t_allocationFailed = false;
			t_failIn = k;
			stubPrepareDepot(nullptr, nullptr, 42, 4201, 7, nullptr);
			t_failIn = 0;
			t_failNextAllocation = false;
			const char res[2] = {
				static_cast<char>(t_allocationFailed),
				static_cast<char>(g_plannerReceivedGid == g_leafReceivedGid && g_plannerReceivedGid != 0),
			};
			(void)!write(pipeFds[1], res, 2);
			std::_Exit(0);
		}
		close(pipeFds[1]);
		char res[2] = {0, 0};
		const bool okRead = read(pipeFds[0], res, 2) == 2;
		close(pipeFds[0]);
		int status = 0;
		waitpid(pid, &status, 0);
		if (!okRead || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			nestedConsistent = false;
			break;
		}
		if (!res[0]) break;
		++nestedRounds;
		if (!res[1])
		{
			nestedConsistent = false;
			std::printf("allocation %d: planner and nested leaf disagreed on gid\n", k);
			break;
		}
	}
	CHECK(nestedRounds > 0 && nestedConsistent,
	      "an allocation failure in PrepareDepotDownload never makes the nested leaf pick another gid");

	// LaunchApp's work (the ticket, the SteamStub check) runs on Steam's IPC
	// thread before the launch: a failure there must not stop the launch.
	int launchRounds = 0;
	bool launchSafe = true;
	for (int k = 1; k <= 200; ++k)
	{
		t_allocationFailed = false;
		t_failIn = k;
		try { Hooks::launchAppWork(nullptr, 4242, 0, 0, nullptr); }
		catch (...) { launchSafe = false; }
		t_failIn = 0;
		t_failNextAllocation = false;
		if (!t_allocationFailed) break;
		++launchRounds;
	}
	CHECK(launchRounds > 0 && launchSafe,
	      "an allocation failure in LaunchApp's work does not reach Steam's launch");

	if (g_failures == 0) std::printf("\nall hook placement checks passed\n");
	else std::printf("\n%d hook placement check(s) FAILED\n", g_failures);
	std::fflush(stdout);
	std::_Exit(g_failures == 0 ? 0 : 1);
}
