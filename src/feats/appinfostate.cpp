// SPDX-License-Identifier: AGPL-3.0-only
//
// Scoped CAppInfoCache metadata guard for live library refresh.

#include "appinfostate.hpp"

#include "../memhlp.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace
{
using Store = HotReloadState::Store;
using Layout = AppDataLayout::Layout;
using PatchState = AppInfoState::PatchState;

using GetOrAddAppDataFn = void* (__attribute__((cdecl))) (
	void*, std::uint32_t, bool
);
using GetOrAddAppDataPtr = GetOrAddAppDataFn*;
using ReadFromDiskPtr = AppInfoReload::ReadFromDiskFn;

constexpr std::size_t kDetourBytes = PatchState::kDetourBytes;
constexpr std::size_t kTrampolineBytes = kDetourBytes * 2;
constexpr unsigned int kMemoryRetryCount = 3;

std::atomic<Store*> g_store{nullptr};
Store g_authoritativeStore;
std::atomic<std::uint32_t> g_skipOffset{0};
std::atomic<std::uint32_t> g_shaOffset{0};
std::atomic<GetOrAddAppDataPtr> g_original{nullptr};
std::atomic<ReadFromDiskPtr> g_readFromDisk{nullptr};
std::atomic<void*> g_cache{nullptr};

static_assert(std::atomic<Store*>::is_always_lock_free,
	"the appinfo store handoff must stay lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
	"the appinfo layout offsets must stay lock-free");
static_assert(std::atomic<GetOrAddAppDataPtr>::is_always_lock_free,
	"the appinfo trampoline handoff must stay lock-free");
static_assert(std::atomic<ReadFromDiskPtr>::is_always_lock_free,
	"the appinfo disk-reader handoff must stay lock-free");
static_assert(std::atomic<void*>::is_always_lock_free,
	"the live appinfo cache handoff must stay lock-free");

AppInfoState::Lifecycle g_lifecycle;
AppInfoState::PatchState g_patch;
lm_address_t g_target = LM_ADDRESS_BAD;
lm_address_t g_trampoline = LM_ADDRESS_BAD;
lm_size_t g_hookSize = 0;
lm_prot_t g_targetBaselineProtection = LM_PROT_NONE;
bool g_haveTargetBaselineProtection = false;
bool g_trampolineLeaked = false;
std::mutex g_lifecycleMutex;

struct ReaderScope
{
	ReaderScope() noexcept { g_lifecycle.enterReader(); }
	~ReaderScope() noexcept { g_lifecycle.leaveReader(); }
};

bool validCodeRange(lm_address_t address, std::size_t size) noexcept
{
	if (address == 0 || address == LM_ADDRESS_BAD || size == 0)
		return false;

	lm_segment_t segment{};
	if (!LM_FindSegment(address, &segment))
		return false;
	if ((segment.prot & LM_PROT_XR) != LM_PROT_XR)
		return false;
	if (address < segment.base || address >= segment.end)
		return false;

	return size <= segment.end - address;
}

bool captureBaselineProtection(
	lm_address_t address,
	std::size_t size,
	lm_prot_t& protection
) noexcept
{
	lm_segment_t segment{};
	if (!LM_FindSegment(address, &segment) ||
		(segment.prot & LM_PROT_XR) != LM_PROT_XR ||
		address < segment.base || address >= segment.end ||
		size > segment.end - address)
	{
		return false;
	}

	protection = segment.prot;
	return true;
}

bool validPattern(const Pattern_t& pattern, std::size_t bytes) noexcept
{
	return validCodeRange(pattern.address, bytes);
}

std::uint32_t readU32(const std::uint8_t* bytes) noexcept
{
	return static_cast<std::uint32_t>(bytes[0]) |
		(static_cast<std::uint32_t>(bytes[1]) << 8) |
		(static_cast<std::uint32_t>(bytes[2]) << 16) |
		(static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool rel32Target(
	lm_address_t instruction,
	const std::array<std::uint8_t, kDetourBytes>& bytes,
	lm_address_t& target
) noexcept
{
	const std::int64_t displacement =
		static_cast<std::int32_t>(readU32(bytes.data() + 1));
	const std::int64_t next =
		static_cast<std::int64_t>(instruction + kDetourBytes);
	const std::int64_t resolved = next + displacement;
	if (resolved < 0 ||
		static_cast<std::uint64_t>(resolved) >
			static_cast<std::uint64_t>(std::numeric_limits<lm_address_t>::max()))
	{
		return false;
	}

	target = static_cast<lm_address_t>(resolved);
	return true;
}

bool buildRel32Jump(
	lm_address_t from,
	lm_address_t to,
	std::array<std::uint8_t, kDetourBytes>& output
) noexcept
{
	const std::int64_t next =
		static_cast<std::int64_t>(from + kDetourBytes);
	const std::int64_t delta =
		static_cast<std::int64_t>(to) - next;
	if (delta < std::numeric_limits<std::int32_t>::min() ||
		delta > std::numeric_limits<std::int32_t>::max())
	{
		return false;
	}

	const std::uint32_t encoded = static_cast<std::uint32_t>(
		static_cast<std::int32_t>(delta));
	output[0] = 0xE9;
	output[1] = static_cast<std::uint8_t>(encoded);
	output[2] = static_cast<std::uint8_t>(encoded >> 8);
	output[3] = static_cast<std::uint8_t>(encoded >> 16);
	output[4] = static_cast<std::uint8_t>(encoded >> 24);
	return true;
}

bool readExact(
	lm_address_t address,
	std::uint8_t* output,
	std::size_t size
) noexcept
{
	return address != 0 && address != LM_ADDRESS_BAD && output != nullptr &&
		LM_ReadMemory(address, output, size) == size;
}

bool sameBytes(
	const std::uint8_t* left,
	const std::uint8_t* right,
	std::size_t size
) noexcept
{
	for (std::size_t index = 0; index < size; ++index)
	{
		if (left[index] != right[index])
			return false;
	}
	return true;
}

bool validateDirectPicThunkCall(
	lm_address_t target,
	const std::array<std::uint8_t, kDetourBytes>& original
) noexcept
{
	if (LM_CodeLength(target, kDetourBytes) != kDetourBytes ||
		original[0] != 0xE8)
	{
		return false;
	}

	lm_inst_t call{};
	if (!LM_Disassemble(target, &call) || call.address != target ||
		call.size != kDetourBytes || std::strcmp(call.mnemonic, "call") != 0 ||
		!sameBytes(call.bytes, original.data(), original.size()))
	{
		return false;
	}

	lm_address_t thunk = LM_ADDRESS_BAD;
	if (!rel32Target(target, original, thunk) ||
		!validCodeRange(thunk, 1))
	{
		return false;
	}

	lm_inst_t thunkMove{};
	if (!LM_Disassemble(thunk, &thunkMove) ||
		std::strcmp(thunkMove.mnemonic, "mov") != 0 ||
		thunkMove.size == 0 ||
		!validCodeRange(thunk + thunkMove.size, 1))
	{
		return false;
	}

	lm_inst_t thunkReturn{};
	return LM_Disassemble(thunk + thunkMove.size, &thunkReturn) &&
		std::strcmp(thunkReturn.mnemonic, "ret") == 0;
}

bool validateTrampoline(
	lm_address_t trampoline,
	lm_address_t target,
	const std::array<std::uint8_t, kDetourBytes>& returnJump
) noexcept
{
	if (!validCodeRange(trampoline, kTrampolineBytes))
		return false;

	std::array<std::uint8_t, kTrampolineBytes> bytes{};
	if (!readExact(trampoline, bytes.data(), bytes.size()) ||
		!sameBytes(bytes.data() + kDetourBytes,
			returnJump.data(), returnJump.size()))
	{
		return false;
	}

	lm_inst_t relocated{};
	if (!LM_Disassemble(trampoline, &relocated) ||
		relocated.address != trampoline || relocated.size != kDetourBytes ||
		// A verified PIC thunk repair replaces the direct call in the copied
		// prologue.  Leaving E8 here would execute the stale relative target.
		relocated.bytes[0] == 0xE8)
	{
		return false;
	}

	lm_inst_t jump{};
	if (!LM_Disassemble(trampoline + kDetourBytes, &jump) ||
		jump.address != trampoline + kDetourBytes ||
		jump.size != kDetourBytes || std::strcmp(jump.mnemonic, "jmp") != 0 ||
		jump.bytes[0] != 0xE9)
	{
		return false;
	}

	lm_address_t jumpTarget = LM_ADDRESS_BAD;
	return rel32Target(trampoline + kDetourBytes,
		std::array<std::uint8_t, kDetourBytes>{
			jump.bytes[0], jump.bytes[1], jump.bytes[2], jump.bytes[3],
			jump.bytes[4]}, jumpTarget) && jumpTarget == target + kDetourBytes;
}

bool protectionMatches(
	lm_address_t address,
	std::size_t size,
	lm_prot_t expected
) noexcept
{
	lm_segment_t segment{};
	return LM_FindSegment(address, &segment) &&
		address >= segment.base && address < segment.end &&
		size <= segment.end - address && segment.prot == expected;
}

bool restoreProtectionOnce(
	lm_address_t address,
	std::size_t size,
	lm_prot_t baselineProtection
) noexcept
{
	return LM_ProtMemory(address, size, baselineProtection, nullptr) &&
		protectionMatches(address, size, baselineProtection);
}

lm_address_t adapterAddress(void* context) noexcept
{
	if (context == nullptr)
		return LM_ADDRESS_BAD;
	return *static_cast<const lm_address_t*>(context);
}

bool adapterMakeWritable(void* context, std::size_t size) noexcept
{
	const lm_address_t address = adapterAddress(context);
	return address != 0 && address != LM_ADDRESS_BAD &&
		LM_ProtMemory(address, size, LM_PROT_XRW, nullptr);
}

std::size_t adapterWrite(
	void* context,
	const std::uint8_t* bytes,
	std::size_t size
) noexcept
{
	const lm_address_t address = adapterAddress(context);
	if (address == 0 || address == LM_ADDRESS_BAD || bytes == nullptr)
		return 0;
	return LM_WriteMemory(address, bytes, size);
}

bool adapterRead(
	void* context,
	std::uint8_t* output,
	std::size_t size
) noexcept
{
	const lm_address_t address = adapterAddress(context);
	return readExact(address, output, size);
}

bool adapterRestoreBaseline(
	void* context,
	AppInfoState::ProtectionBaseline baseline
) noexcept
{
	const lm_address_t address = adapterAddress(context);
	return address != 0 && address != LM_ADDRESS_BAD && baseline.valid &&
		restoreProtectionOnce(
			address, PatchState::kDetourBytes,
			static_cast<lm_prot_t>(baseline.value));
}

AppInfoState::WriteOutcome writeTargetBytes(
	lm_address_t address,
	const std::uint8_t* bytes,
	std::size_t size,
	AppInfoState::ProtectionBaseline baseline
) noexcept
{
	if (address == 0 || address == LM_ADDRESS_BAD || bytes == nullptr ||
		size != kDetourBytes || !baseline.valid)
	{
		return AppInfoState::WriteOutcome{};
	}

	const AppInfoState::TargetWriteCallbacks callbacks{
		&adapterMakeWritable, &adapterWrite, &adapterRead,
		&adapterRestoreBaseline};
	return AppInfoState::writeWithRetainedBaseline(
		callbacks, &address, bytes, size, baseline, kMemoryRetryCount);
}

bool targetRead(void* context, std::uint8_t* output, std::size_t size) noexcept
{
	if (context == nullptr || g_hookSize != kDetourBytes)
		return false;
	const auto target = *static_cast<const lm_address_t*>(context);
	return readExact(target, output, size);
}

AppInfoState::WriteOutcome targetWrite(
	void* context,
	const std::uint8_t* bytes,
	std::size_t size,
	AppInfoState::ProtectionBaseline baseline
) noexcept
{
	if (context == nullptr || g_hookSize != kDetourBytes)
		return AppInfoState::WriteOutcome{};
	const auto target = *static_cast<const lm_address_t*>(context);
	return writeTargetBytes(target, bytes, size, baseline);
}

AppInfoState::ByteAccess targetAccess() noexcept
{
	if (!g_haveTargetBaselineProtection ||
		g_targetBaselineProtection == LM_PROT_NONE)
		return {};
	return AppInfoState::ByteAccess{
		&g_target, &targetRead, &targetWrite};
}

bool releaseTrampolineLocked() noexcept
{
	if (g_trampoline == LM_ADDRESS_BAD || g_trampoline == 0)
		return true;

	const bool released = LM_FreeMemory(g_trampoline, 0);
	if (!released)
		g_trampolineLeaked = true;
	g_trampoline = LM_ADDRESS_BAD;
	return released;
}

bool discardBeforePatchLocked() noexcept
{
	// The target is still byte-for-byte original on this path.  It is therefore
	// safe to clear the published trampoline before attempting the best-effort
	// free; a failed free is only an unreachable setup leak.
	g_store.store(nullptr, std::memory_order_release);
	g_original.store(nullptr, std::memory_order_release);
	g_readFromDisk.store(nullptr, std::memory_order_release);
	g_cache.store(nullptr, std::memory_order_release);
	g_target = LM_ADDRESS_BAD;
	g_hookSize = 0;
	g_targetBaselineProtection = LM_PROT_NONE;
	g_haveTargetBaselineProtection = false;
	g_patch.clear();
	const bool released = releaseTrampolineLocked();
	g_lifecycle.failBeforeHook();
	return released;
}

void discardAfterVerifiedRestoreLocked() noexcept
{
	// The target is known original and no hook reader remains.  Only now may
	// the trampoline/function pointer be cleared and the executable page freed.
	g_store.store(nullptr, std::memory_order_release);
	g_original.store(nullptr, std::memory_order_release);
	g_readFromDisk.store(nullptr, std::memory_order_release);
	g_cache.store(nullptr, std::memory_order_release);
	g_target = LM_ADDRESS_BAD;
	g_hookSize = 0;
	g_targetBaselineProtection = LM_PROT_NONE;
	g_haveTargetBaselineProtection = false;
	g_patch.clear();
	(void)releaseTrampolineLocked();
}

void* __attribute__((cdecl)) hkGetOrAddAppData(
	void* cache,
	std::uint32_t appId,
	bool create
) noexcept
{
	// Hooks::setup/remove are externally quiescent code-patch boundaries.  The
	// counter nevertheless spans the original call and all guarded work so a
	// Store, layout, or trampoline cannot be reclaimed under an active entry.
	ReaderScope reader;
	if (cache != nullptr)
		g_cache.store(cache, std::memory_order_release);

	const GetOrAddAppDataPtr original =
		g_original.load(std::memory_order_acquire);
	void* const data = original(cache, appId, create);
	if (data == nullptr || create || !g_lifecycle.managed())
		return data;

	Store* const store = g_store.load(std::memory_order_acquire);
	if (store == nullptr)
		return data;

	const Layout layout{
		g_skipOffset.load(std::memory_order_acquire),
		g_shaOffset.load(std::memory_order_acquire)};
	auto read = store->readHandle();
	auto authoritativeRead = g_authoritativeStore.readHandle();
	return AppInfoState::guard(
		data, appId, create, read, authoritativeRead, layout);
}

// Deliberately not noexcept: PIC repair may allocate through libmem helpers.
// This function reaches that potentially-throwing work before the lifecycle
// is provisional and before any target byte is changed; setup's outer catch
// reacquires the lifecycle mutex only after this scope has unwound.
bool installLocked(Store& store)
{
	const Pattern_t& functionPattern = Patterns::CAppInfoCache::GetOrAddAppData;
	const Pattern_t& readPattern = Patterns::CAppInfoCache::ThreadedReadFromDisk;
	const Pattern_t& skipPattern = Patterns::CAppInfoCache::SkipFlagReference;
	const Pattern_t& shaPattern = Patterns::CAppInfoCache::ShaReference;
	if (!validPattern(functionPattern, kDetourBytes) ||
		!validPattern(skipPattern, 4) || !validPattern(shaPattern, 3))
	{
		discardBeforePatchLocked();
		return false;
	}

	std::array<std::uint8_t, 4> skipInstruction{};
	std::array<std::uint8_t, 3> shaInstruction{};
	std::array<std::uint8_t, kDetourBytes> original{};
	lm_prot_t baselineProtection = LM_PROT_NONE;
	if (!readExact(skipPattern.address, skipInstruction.data(), skipInstruction.size()) ||
		!readExact(shaPattern.address, shaInstruction.data(), shaInstruction.size()) ||
		!readExact(functionPattern.address, original.data(), original.size()) ||
		!validateDirectPicThunkCall(functionPattern.address, original) ||
		!captureBaselineProtection(
			functionPattern.address, kDetourBytes, baselineProtection))
	{
		discardBeforePatchLocked();
		return false;
	}

	const auto derived = AppDataLayout::derive(skipInstruction, shaInstruction);
	if (!derived.has_value())
	{
		discardBeforePatchLocked();
		return false;
	}

	std::array<std::uint8_t, kDetourBytes> hookJump{};
	if (!buildRel32Jump(functionPattern.address,
		reinterpret_cast<lm_address_t>(&hkGetOrAddAppData), hookJump))
	{
		discardBeforePatchLocked();
		return false;
	}

	const lm_address_t trampoline = LM_AllocMemory(0, LM_PROT_XRW);
	if (trampoline == LM_ADDRESS_BAD || trampoline == 0 ||
		!validCodeRange(trampoline, kTrampolineBytes))
	{
		if (trampoline != LM_ADDRESS_BAD && trampoline != 0)
		{
			g_trampoline = trampoline;
			(void)releaseTrampolineLocked();
		}
		discardBeforePatchLocked();
		return false;
	}

	// Retain every provisional detour field before any target mutation.
	g_target = functionPattern.address;
	g_trampoline = trampoline;
	g_hookSize = kDetourBytes;
	g_targetBaselineProtection = baselineProtection;
	g_haveTargetBaselineProtection = true;

	std::array<std::uint8_t, kDetourBytes> returnJump{};
	if (!buildRel32Jump(trampoline + kDetourBytes,
		functionPattern.address + kDetourBytes, returnJump) ||
		LM_WriteMemory(trampoline, original.data(), original.size()) !=
			original.size() ||
		LM_WriteMemory(trampoline + kDetourBytes,
			returnJump.data(), returnJump.size()) != returnJump.size())
	{
		discardBeforePatchLocked();
		return false;
	}

	std::array<std::uint8_t, kTrampolineBytes> copied{};
	if (!readExact(trampoline, copied.data(), copied.size()) ||
		!sameBytes(copied.data(), original.data(), original.size()) ||
		!sameBytes(copied.data() + kDetourBytes,
			returnJump.data(), returnJump.size()) ||
		!MemHlp::fixPICThunkCall(
			functionPattern.name.c_str(), functionPattern.address, trampoline) ||
		!validateTrampoline(trampoline, functionPattern.address, returnJump))
	{
		discardBeforePatchLocked();
		return false;
	}

	// The callable trampoline and immutable offsets are published before the
	// target's first byte can redirect a Steam thread into this hook.
	g_skipOffset.store(derived->skipOffset, std::memory_order_release);
	g_shaOffset.store(derived->shaOffset, std::memory_order_release);
	g_store.store(&store, std::memory_order_release);
	g_original.store(
		reinterpret_cast<GetOrAddAppDataPtr>(trampoline),
		std::memory_order_release);
	g_readFromDisk.store(
		validPattern(readPattern, 1)
			? reinterpret_cast<ReadFromDiskPtr>(readPattern.address)
			: nullptr,
		std::memory_order_release);
	g_patch.configure(
		original,
		hookJump,
		AppInfoState::ProtectionBaseline{
			static_cast<std::uint32_t>(baselineProtection), true});
	g_lifecycle.markProvisional(g_original.load(std::memory_order_acquire) != nullptr);

	const AppInfoState::PatchResult result = g_patch.patch(targetAccess());
	if (result == AppInfoState::PatchResult::Applied)
	{
		if (g_lifecycle.activate())
			return true;

		// Activation is a post-patch validation boundary.  Restore through the
		// same checked helper before any trampoline is released.
		const auto rollback = g_patch.restore(targetAccess());
		if (rollback == AppInfoState::PatchResult::Restored ||
			rollback == AppInfoState::PatchResult::Untouched)
		{
			(void)g_lifecycle.finishUnhook(true);
			discardAfterVerifiedRestoreLocked();
		}
		else if (rollback == AppInfoState::PatchResult::Catastrophic)
		{
			g_store.store(nullptr, std::memory_order_release);
			g_lifecycle.markCatastrophic();
		}
		else
		{
			g_store.store(nullptr, std::memory_order_release);
			(void)g_lifecycle.finishUnhook(false);
		}
		return false;
	}

	g_store.store(nullptr, std::memory_order_release);
	if (result == AppInfoState::PatchResult::Restored ||
		result == AppInfoState::PatchResult::Untouched)
	{
		(void)g_lifecycle.finishUnhook(true);
		discardAfterVerifiedRestoreLocked();
	}
	else if (result == AppInfoState::PatchResult::Catastrophic)
	{
		g_lifecycle.markCatastrophic();
	}
	else
	{
		(void)g_lifecycle.finishUnhook(false);
	}
	return false;
}
}

namespace AppInfoState
{
bool setup(Store& store) noexcept
{
	try
	{
		return AppInfoState::detail::withLifecycleLock(
			g_lifecycleMutex,
			[&]() -> bool
		{
			if (g_lifecycle.canRebind())
			{
				// The bootstrap and coordinator Stores are non-owning bindings.  Both
				// must outlive the installed hook and every retired reader.
				g_store.store(&store, std::memory_order_release);
				return true;
			}
			if (g_lifecycle.installAttempted())
				return false;
			if (!g_lifecycle.beginInstall())
				return false;
			return installLocked(store);
		}
		);
	}
	catch (...)
	{
		// A setup-time mutex/system_error, allocation, or decoder exception must
		// not make this optional hook abort global Hooks::setup.  No exception is
		// reachable from hkGetOrAddAppData.
		try
		{
			std::lock_guard<std::mutex> lock(g_lifecycleMutex);
			if (!g_lifecycle.hookInstalled())
			{
				discardBeforePatchLocked();
			}
			else
			{
				g_lifecycle.disableManaged();
				g_store.store(nullptr, std::memory_order_release);
			}
		}
		catch (...)
		{
			g_lifecycle.disableManaged();
			g_store.store(nullptr, std::memory_order_release);
		}
		return false;
	}
}

void remove() noexcept
{
	// This is the explicit quiescent Hooks::remove contract: no new call may
	// enter the patched target after disable, and existing hook entries drain
	// before the five target bytes are restored.
	g_lifecycle.disableManaged();
	g_store.store(nullptr, std::memory_order_release);

	try
	{
		std::lock_guard<std::mutex> lock(g_lifecycleMutex);
		if (!g_lifecycle.hookInstalled())
			return;

		while (!g_lifecycle.readersDrained())
			std::this_thread::yield();

		const PatchResult result = g_patch.restore(targetAccess());
		if (result == PatchResult::Restored ||
			result == PatchResult::Untouched)
		{
			(void)g_lifecycle.finishUnhook(true);
			discardAfterVerifiedRestoreLocked();
		}
		else if (result == PatchResult::Catastrophic)
		{
			g_lifecycle.markCatastrophic();
		}
		else
		{
			// Exact hook bytes remain and the valid trampoline is retained.  A
			// later quiescent remove can retry restoration; managed work stays off.
			(void)g_lifecycle.finishUnhook(false);
		}
	}
	catch (...)
	{
		// Preserve target/trampoline/original metadata on every teardown failure;
		// losing it while the target may still jump here would not be fail-open.
		g_lifecycle.disableManaged();
		g_store.store(nullptr, std::memory_order_release);
	}
}

bool ready() noexcept
{
	return g_lifecycle.managed();
}

bool catastrophic() noexcept
{
	return g_lifecycle.catastrophic();
}

bool resolvedDirtyHint() noexcept
{
	Store* const store = g_store.load(std::memory_order_acquire);
	return store != nullptr && store->resolvedDirtyHint();
}

bool takeResolvedDirty() noexcept
{
	Store* const store = g_store.load(std::memory_order_acquire);
	if (store == nullptr)
		return false;
	try
	{
		return store->takeResolvedDirty();
	}
	catch (...)
	{
		return false;
	}
}

void publishAuthoritative(
	const std::unordered_set<std::uint32_t>& appIds) noexcept
{
	try
	{
		(void)g_authoritativeStore.publish(appIds);
	}
	catch (...)
	{
		// Keep the previous complete authority set on allocation failure.
	}
}

bool isAuthoritative(std::uint32_t appId) noexcept
{
	try
	{
		auto read = g_authoritativeStore.readHandle();
		return read.contains(appId);
	}
	catch (...)
	{
		return false;
	}
}

AppInfoReload::Result reloadFromDisk(
	std::span<const std::uint32_t> appIds) noexcept
{
	ReaderScope reader;
	if (!g_lifecycle.managed() || appIds.empty())
		return {};

	Store* const store = g_store.load(std::memory_order_acquire);
	const auto readFromDisk = g_readFromDisk.load(std::memory_order_acquire);
	const auto lookup = g_original.load(std::memory_order_acquire);
	void* const cache = g_cache.load(std::memory_order_acquire);
	if (store == nullptr || readFromDisk == nullptr || lookup == nullptr ||
		cache == nullptr)
	{
		return {};
	}

	const Layout layout{
		g_skipOffset.load(std::memory_order_acquire),
		g_shaOffset.load(std::memory_order_acquire)};
	return AppInfoReload::reload(
		AppInfoReload::Runtime{cache, readFromDisk, lookup},
		appIds, *store, layout);
}
}
