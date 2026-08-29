#include "../src/feats/appinfostate.hpp"
#include "../src/feats/appinfostate_policy.hpp"
#include "../src/feats/appdata_layout.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <new>
#include <string_view>

namespace
{
using AppInfoStatePolicy::Action;
using AppInfoStatePolicy::decide;
using AppInfoState::Lifecycle;
using AppInfoState::LifecyclePhase;
using AppInfoState::PatchResult;
using AppInfoState::PatchState;
using AppInfoState::ProtectionBaseline;
using AppInfoState::WriteOutcome;

int failures = 0;

constexpr ProtectionBaseline kOriginalProtection{5, true};
constexpr std::uint32_t kPostFailureProtection = 7;

bool check(bool condition, std::string_view message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++failures;
		return false;
	}
	return true;
}

bool test_guard_with_real_store()
{
	bool ok = true;
	HotReloadState::Store store;
	HotReloadState::Store authoritative;
	store.publish({101});
	authoritative.publish({});
	constexpr AppDataLayout::Layout layout{0x10, 0x1C};
	std::array<std::uint8_t, 64> data{};

	{
		auto read = store.readHandle();
		ok &= check(AppInfoState::guard(nullptr, 101, false, read, layout) == nullptr,
			"null result is returned untouched before policy");
		ok &= check(AppInfoState::guard(data.data(), 101, true, read, layout) == data.data(),
			"creating lookup is returned untouched");
	}

	{
		auto read = store.readHandle();
		ok &= check(AppInfoState::guard(data.data(), 999, false, read, layout) == data.data(),
			"unmanaged app is returned untouched");
	}

	data[layout.skipOffset] = 0;
	{
		auto read = store.readHandle();
		ok &= check(AppInfoState::guard(data.data(), 101, false, read, layout) == data.data(),
			"managed unresolved result is returned after marking");
		ok &= check(data[layout.skipOffset] == 1,
			"managed unresolved result receives the derived skip byte");
	}
	{
		auto read = store.readHandle();
		ok &= check(AppInfoState::guard(data.data(), 101, false, read, layout) == data.data(),
			"already-marked unresolved result remains idempotent");
	}
	ok &= check(!store.takeResolvedDirty(),
		"unresolved skip marking does not signal a resolved generation");

	data[layout.skipOffset] = 0;
	data[layout.shaOffset] = 0xA5;
	{
		auto read = store.readHandle();
		ok &= check(AppInfoState::guard(data.data(), 101, false, read, layout) == data.data(),
			"resolved result is returned untouched");
	}
	ok &= check(store.takeResolvedDirty(),
		"resolved metadata raises the generation dirty signal");
	ok &= check(!store.takeResolvedDirty(),
		"resolved dirty signal is idempotently consumed");

	// A synthetic record is locally authoritative even after its live reload
	// has installed a real SHA. Steam's update state must retain the skip byte
	// while still notifying the hot-reload coordinator that metadata resolved.
	store.publish({});
	store.publish({202});
	authoritative.publish({202});
	data.fill(0);
	data[layout.shaOffset] = 0x5a;
	{
		auto managedRead = store.readHandle();
		auto authoritativeRead = authoritative.readHandle();
		ok &= check(AppInfoState::guard(
			data.data(), 202, false, managedRead, authoritativeRead, layout) ==
			data.data(),
			"resolved authoritative result is returned after policy");
	}
	ok &= check(data[layout.skipOffset] == 1,
		"resolved authoritative appinfo retains the derived skip byte");
	ok &= check(store.takeResolvedDirty(),
		"resolved authoritative appinfo also signals its generation");

	store.publish({});
	store.publish({101});
	data.fill(0);
	{
		auto stale = store.readHandle();
		ok &= check(stale.contains(101),
			"old handle retains the removed generation safely");
		store.publish({});
		store.publish({101});
		ok &= check(stale.contains(101) && !stale.noteResolved(),
			"old handle cannot resolve a re-added generation");
	}
	{
		auto fresh = store.readHandle();
		ok &= check(fresh.contains(101) && fresh.noteResolved(),
			"fresh re-added state resolves exactly once");
	}
	ok &= check(store.takeResolvedDirty(),
		"fresh re-added resolution raises the current dirty signal");
	return ok;
}

struct PatchBytes
{
	PatchState::Bytes current{};
	WriteOutcome writeOutcome{true, true, true, true};
	std::uint32_t actualProtection = kOriginalProtection.value;
	std::uint32_t protectionAfterWrite = kOriginalProtection.value;
	bool mutateOnWrite = true;
	bool corruptOnWrite = false;
	unsigned int readFailuresAfterWrite = 0;
	unsigned int writes = 0;
};

bool patchRead(void* context, std::uint8_t* output, std::size_t size) noexcept
{
	if (context == nullptr || output == nullptr || size != PatchState::kDetourBytes)
		return false;
	auto& bytes = *static_cast<PatchBytes*>(context);
	if (bytes.writes != 0 && bytes.readFailuresAfterWrite != 0)
	{
		--bytes.readFailuresAfterWrite;
		return false;
	}
	std::memcpy(output, bytes.current.data(), size);
	return true;
}

WriteOutcome patchWrite(
	void* context,
	const std::uint8_t* input,
	std::size_t size,
	ProtectionBaseline baseline
) noexcept
{
	if (context == nullptr || input == nullptr || size != PatchState::kDetourBytes)
		return WriteOutcome{};
	auto& bytes = *static_cast<PatchBytes*>(context);
	++bytes.writes;
	if (bytes.mutateOnWrite)
	{
		std::memcpy(bytes.current.data(), input, size);
		bytes.actualProtection = bytes.protectionAfterWrite;
		if (bytes.corruptOnWrite)
			bytes.current[0] ^= 0x7f;
	}
	WriteOutcome outcome = bytes.writeOutcome;
	if (outcome.originalProtectionVerified)
		outcome.originalProtectionVerified =
			bytes.actualProtection == baseline.value;
	return outcome;
}

bool test_patch_state_machine()
{
	constexpr PatchState::Bytes original{{0x55, 0x89, 0xe5, 0x57, 0x56}};
	constexpr PatchState::Bytes hook{{0xe9, 0x01, 0x02, 0x03, 0x04}};
	bool ok = true;

	PatchState state;
	state.configure(original, hook, kOriginalProtection);
	PatchBytes bytes{original};
	const AppInfoState::ByteAccess bound{&bytes, &patchRead, &patchWrite};
	ok &= check(state.patch(bound) == PatchResult::Applied &&
		state.installed(),
		"checked patch accepts exact hook bytes");
	ok &= check(state.restore(bound) == PatchResult::Restored &&
		state.freeEligible() && bytes.current == original,
		"verified restore makes trampoline free-eligible");
	ok &= check(state.restore(bound) == PatchResult::Restored,
		"verified restore is idempotent");

	PatchState rollback;
	rollback.configure(original, hook, kOriginalProtection);
	PatchBytes failedWrite{original};
	failedWrite.writeOutcome = WriteOutcome{true, false, false, false};
	failedWrite.mutateOnWrite = false;
	failedWrite.readFailuresAfterWrite = 1;
	const auto failedAccess = AppInfoState::ByteAccess{
		&failedWrite, &patchRead, &patchWrite};
	ok &= check(rollback.patch(failedAccess) == PatchResult::Retained &&
		!rollback.freeEligible() &&
		failedWrite.current == original,
		"failed patch keeps metadata when rollback protection is unverified");
	failedWrite.writeOutcome = WriteOutcome{true, true, true, true};
	ok &= check(rollback.restore(failedAccess) == PatchResult::Restored &&
		rollback.freeEligible(),
		"rollback protection ambiguity becomes free-eligible only after retry");

	PatchState retained;
	retained.configure(original, hook, kOriginalProtection);
	PatchBytes retainedBytes{original};
	retainedBytes.writeOutcome = WriteOutcome{true, true, true, false};
	const auto retainedAccess = AppInfoState::ByteAccess{
		&retainedBytes, &patchRead, &patchWrite};
	// The target is exact-hook after the checked write but protection restoration
	// is unverified: retain fail-open metadata and never free the trampoline.
	ok &= check(retained.patch(retainedAccess) == PatchResult::Retained &&
		retained.installed() && !retained.freeEligible(),
		"unverified hook protection retains an exact hook safely");
	retainedBytes.writeOutcome = WriteOutcome{true, true, true, false};
	ok &= check(retained.restore(retainedAccess) == PatchResult::Retained &&
		!retained.freeEligible() && retainedBytes.current == original,
		"original readback alone cannot free after unverified protection");
	retainedBytes.writeOutcome = WriteOutcome{true, true, true, true};
	ok &= check(retained.restore(retainedAccess) == PatchResult::Restored &&
		retained.freeEligible(),
		"retained hook can be restored on a later retry");

	PatchState untouched;
	untouched.configure(original, hook, kOriginalProtection);
	PatchBytes untouchedBytes{original};
	untouchedBytes.writeOutcome = WriteOutcome{};
	untouchedBytes.mutateOnWrite = false;
	const auto untouchedAccess = AppInfoState::ByteAccess{
		&untouchedBytes, &patchRead, &patchWrite};
	ok &= check(untouched.patch(untouchedAccess) == PatchResult::Untouched &&
		untouched.freeEligible() && untouchedBytes.writes == 1,
		"proven pre-write protection failure is distinct from restoration");

	PatchState corrupt;
	corrupt.configure(original, hook, kOriginalProtection);
	PatchBytes corruptBytes{{0x90, 0x90, 0x90, 0x90, 0x90}};
	const auto corruptAccess = AppInfoState::ByteAccess{
		&corruptBytes, &patchRead, &patchWrite};
	ok &= check(corrupt.patch(corruptAccess) == PatchResult::Catastrophic &&
		corrupt.catastrophic() && corruptBytes.writes == 0,
		"neither-original-nor-hook bytes refuse patching");
	ok &= check(corrupt.restore(corruptAccess) == PatchResult::Catastrophic,
		"catastrophic state refuses unsafe restoration");

	return ok;
}

struct WriterBytes
{
	PatchState::Bytes current{};
	std::uint32_t actualProtection = kOriginalProtection.value;
	bool restoreToBaseline = false;
	std::array<std::uint32_t, 16> requestedBaselines{};
	std::size_t restoreCalls = 0;
};

bool writerMakeWritable(void* context, std::size_t size) noexcept
{
	if (context == nullptr || size != PatchState::kDetourBytes)
		return false;
	static_cast<WriterBytes*>(context)->actualProtection =
		kPostFailureProtection;
	return true;
}

std::size_t writerWrite(
	void* context,
	const std::uint8_t* input,
	std::size_t size
) noexcept
{
	if (context == nullptr || input == nullptr ||
		size != PatchState::kDetourBytes)
		return 0;
	std::memcpy(
		static_cast<WriterBytes*>(context)->current.data(), input, size);
	return size;
}

bool writerRead(
	void* context,
	std::uint8_t* output,
	std::size_t size
) noexcept
{
	if (context == nullptr || output == nullptr ||
		size != PatchState::kDetourBytes)
		return false;
	std::memcpy(
		output, static_cast<WriterBytes*>(context)->current.data(), size);
	return true;
}

bool writerRestoreBaseline(
	void* context,
	ProtectionBaseline baseline
) noexcept
{
	if (context == nullptr || !baseline.valid)
		return false;
	auto& bytes = *static_cast<WriterBytes*>(context);
	if (bytes.restoreCalls < bytes.requestedBaselines.size())
		bytes.requestedBaselines[bytes.restoreCalls] = baseline.value;
	++bytes.restoreCalls;
	if (bytes.restoreToBaseline)
		bytes.actualProtection = baseline.value;
	return bytes.actualProtection == baseline.value;
}

bool test_shared_writer_baseline()
{
	constexpr PatchState::Bytes original{{0x55, 0x89, 0xe5, 0x57, 0x56}};
	constexpr PatchState::Bytes hook{{0xe9, 0x01, 0x02, 0x03, 0x04}};
	WriterBytes bytes{original};
	const AppInfoState::TargetWriteCallbacks callbacks{
		&writerMakeWritable, &writerWrite, &writerRead,
		&writerRestoreBaseline};

	const WriteOutcome first = AppInfoState::writeWithRetainedBaseline(
		callbacks, &bytes, hook.data(), hook.size(), kOriginalProtection);
	bool ok = check(first.attempted && first.writeReportedComplete &&
		first.desiredBytesVerified && !first.originalProtectionVerified &&
		bytes.current == hook &&
		bytes.actualProtection == kPostFailureProtection,
		"shared writer retains XRW after failed XR restoration");

	const WriteOutcome second = AppInfoState::writeWithRetainedBaseline(
		callbacks, &bytes, original.data(), original.size(), kOriginalProtection);
	ok &= check(second.attempted && second.writeReportedComplete &&
		second.desiredBytesVerified && !second.originalProtectionVerified &&
		bytes.current == original &&
		bytes.actualProtection == kPostFailureProtection,
		"shared writer rejects current XRW as the retained baseline");
	ok &= check(bytes.restoreCalls == 6,
		"shared writer executes the bounded retry sequence twice");
	for (std::size_t index = 0; index < bytes.restoreCalls; ++index)
	{
		ok &= check(bytes.requestedBaselines[index] == kOriginalProtection.value,
			"every retry requests the original XR baseline");
	}

	bytes.restoreToBaseline = true;
	const WriteOutcome final = AppInfoState::writeWithRetainedBaseline(
		callbacks, &bytes, original.data(), original.size(), kOriginalProtection);
	ok &= check(final.originalProtectionVerified &&
		bytes.actualProtection == kOriginalProtection.value,
		"only verified XR restoration produces a successful writer outcome");
	return ok;
}

bool test_protection_baseline_drift()
{
	constexpr PatchState::Bytes original{{0x55, 0x89, 0xe5, 0x57, 0x56}};
	constexpr PatchState::Bytes hook{{0xe9, 0x01, 0x02, 0x03, 0x04}};
	PatchState state;
	state.configure(original, hook, kOriginalProtection);

	PatchBytes bytes{original};
	// First operation leaves the target in the hook state and fails to restore
	// the real XR baseline.  The second operation models a writer that restores
	// the original bytes and reports success only because it observed the
	// post-failure XRW state as its new baseline.
	bytes.protectionAfterWrite = kPostFailureProtection;
	bytes.writeOutcome = WriteOutcome{true, true, true, false};
	const AppInfoState::ByteAccess access{
		&bytes, &patchRead, &patchWrite};
	bool ok = check(state.patch(access) == PatchResult::Retained,
		"baseline drift setup retains the failed first operation");

	bytes.writeOutcome = WriteOutcome{true, true, true, true};
	ok &= check(state.restore(access) == PatchResult::Retained &&
		!state.freeEligible(),
		"post-failure protection cannot masquerade as the original baseline");

	bytes.protectionAfterWrite = kOriginalProtection.value;
	ok &= check(state.restore(access) == PatchResult::Restored &&
		state.freeEligible(),
		"only the retained original protection baseline permits release");
	return ok;
}

bool test_setup_lock_exception_boundary()
{
	std::mutex mutex;
	bool caught = false;
	try
	{
		AppInfoState::detail::withLifecycleLock(mutex, []() -> bool
		{
			throw std::bad_alloc();
		});
	}
	catch (const std::bad_alloc&)
	{
		caught = true;
	}

	bool reacquired = mutex.try_lock();
	if (reacquired)
		mutex.unlock();
	return check(caught && reacquired,
		"setup-time exception escapes after lifecycle mutex unwinds");
}

bool test_lifecycle_state_machine()
{
	bool ok = true;
	Lifecycle lifecycle;
	ok &= check(lifecycle.phase() == LifecyclePhase::Fresh,
		"lifecycle starts fresh");
	ok &= check(lifecycle.beginInstall(),
		"first install window opens");
	ok &= check(!lifecycle.beginInstall(),
		"install window cannot open twice");
	lifecycle.markProvisional(true);
	ok &= check(lifecycle.hookInstalled() && lifecycle.originalCallable(),
		"provisional state retains callable detour metadata");
	ok &= check(lifecycle.activate() && lifecycle.managed(),
		"provisional state activates managed behavior");
	ok &= check(lifecycle.canRebind(),
		"ready state permits only store rebinding");

	lifecycle.enterReader();
	lifecycle.disableManaged();
	ok &= check(!lifecycle.managed() && !lifecycle.readersDrained(),
		"disable rejects managed work while a reader remains");
	ok &= check(!lifecycle.canRebind() && !lifecycle.quiescentForUnhook(),
		"disabled readers cannot be unhooked yet");
	lifecycle.leaveReader();
	ok &= check(lifecycle.quiescentForUnhook(),
		"reader drain opens the unhook boundary");
	ok &= check(!lifecycle.finishUnhook(false) && lifecycle.hookInstalled(),
		"unhook failure retains a fail-open hook");
	ok &= check(lifecycle.phase() == LifecyclePhase::DisabledHooked &&
		lifecycle.originalCallable() && !lifecycle.managed(),
		"retained failure keeps original callable and disables mutation");
	ok &= check(!lifecycle.beginInstall(),
		"retained failure does not open a late reinstall window");
	ok &= check(lifecycle.finishUnhook(true) && !lifecycle.hookInstalled(),
		"a later successful remove clears retained metadata");
	ok &= check(lifecycle.finishUnhook(true),
		"remove is idempotent after successful unhook");

	Lifecycle rollback;
	ok &= check(rollback.beginInstall(),
		"rollback case opens its one install window");
	rollback.markProvisional(true);
	ok &= check(rollback.finishUnhook(true) &&
		rollback.phase() == LifecyclePhase::Disabled,
		"post-hook validation rollback reaches disabled state");

	Lifecycle catastrophic;
	ok &= check(catastrophic.beginInstall(),
		"catastrophic case opens its one install window");
	catastrophic.markProvisional(true);
	catastrophic.markCatastrophic();
	ok &= check(catastrophic.catastrophic() && !catastrophic.managed(),
		"catastrophic state stays disabled and retained");
	ok &= check(!catastrophic.finishUnhook(true) && catastrophic.hookInstalled(),
		"catastrophic state refuses an unverified clear");
	ok &= check(!catastrophic.beginInstall(),
		"catastrophic state cannot reinstall late");
	return ok;
}
}

int main()
{
	struct PolicyCase
	{
		bool managed;
		bool authoritative;
		bool create;
		bool shaEmpty;
		bool skipSet;
		Action expected;
		std::string_view message;
	};

	// Mutation check: every row is a literal requirement, not a second
	// implementation of decide().
	constexpr std::array<PolicyCase, 17> cases{{
		{false, false, false, false, false, Action::None, "unmanaged/noncreate/nonempty/clear"},
		{false, false, false, false, true,  Action::None, "unmanaged/noncreate/nonempty/set"},
		{false, false, false, true,  false, Action::None, "unmanaged/noncreate/empty/clear"},
		{false, false, false, true,  true,  Action::None, "unmanaged/noncreate/empty/set"},
		{false, false, true,  false, false, Action::None, "unmanaged/create/nonempty/clear"},
		{false, false, true,  false, true,  Action::None, "unmanaged/create/nonempty/set"},
		{false, false, true,  true,  false, Action::None, "unmanaged/create/empty/clear"},
		{false, false, true,  true,  true,  Action::None, "unmanaged/create/empty/set"},
		{true,  false, true,  false, false, Action::None, "managed/create/nonempty/clear"},
		{true,  false, true,  false, true,  Action::None, "managed/create/nonempty/set"},
		{true,  false, true,  true,  false, Action::None, "managed/create/empty/clear"},
		{true,  false, true,  true,  true,  Action::None, "managed/create/empty/set"},
		{true,  false, false, false, false, Action::SignalResolved, "managed/noncreate/nonempty/clear"},
		{true,  false, false, false, true,  Action::SignalResolved, "managed/noncreate/nonempty/set"},
		{true,  false, false, true,  false, Action::MarkSkip, "managed/noncreate/empty/clear"},
		{true,  false, false, true,  true,  Action::None, "managed/noncreate/empty/set"},
		{true,  true,  false, false, false, Action::MarkSkipAndSignalResolved, "authoritative/noncreate/nonempty/clear"},
	}};

	for (const auto& test : cases)
	{
		check(
			decide(test.managed, test.authoritative, test.create,
			       test.shaEmpty, test.skipSet) ==
				test.expected,
			test.message
		);
	}

	check(
		decide(false, false, false, true, false) == Action::None,
		"non-managed app is untouched"
	);
	check(
		decide(true, false, true, true, false) == Action::None,
		"creating lookup is untouched"
	);
	check(
		decide(true, false, false, true, false) == Action::MarkSkip,
		"managed unresolved lookup becomes non-blocking"
	);
	check(
		decide(true, false, false, true, true) == Action::None,
		"already-marked lookup is idempotent"
	);
	check(
		decide(true, false, false, false, false) == Action::SignalResolved,
		"non-empty SHA signals current generation"
	);
	check(
		decide(true, false, false, false, true) == Action::SignalResolved,
		"non-empty SHA signals even when skip is already set"
	);

	check(test_guard_with_real_store(),
		"real Store guard seam covers lifecycle-sensitive metadata behavior");
	check(test_patch_state_machine(),
		"patch seam covers success, rollback, retention, retry, and corruption");
	check(test_shared_writer_baseline(),
		"shared production writer seam covers retained-baseline retries");
	check(test_protection_baseline_drift(),
		"patch seam rejects protection-baseline drift across invocations");
	check(test_setup_lock_exception_boundary(),
		"setup exception seam proves lifecycle lock unwinds before catch");
	check(test_lifecycle_state_machine(),
		"lifecycle seam covers provisional, rollback, retention, and drain states");

	return failures == 0 ? 0 : 1;
}
