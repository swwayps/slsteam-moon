#pragma once

#include "appdata_layout.hpp"
#include "appinforeload.hpp"
#include "appinfostate_policy.hpp"
#include "hotreload_state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_set>
#include <utility>

namespace AppInfoState
{
struct ProtectionBaseline
{
	// Opaque platform protection flags.  Keeping the token as a plain integer
	// keeps the host-test seam independent of libmem while PatchState retains
	// the setup-time value across every retry.
	std::uint32_t value = 0;
	bool valid = false;
};

struct WriteOutcome
{
	// `attempted == false` proves that no protection call succeeded and no
	// target write was reached.  Once attempted, bytes and original protection
	// must be tracked independently; a bool cannot safely represent that state.
	bool attempted = false;
	bool writeReportedComplete = false;
	bool desiredBytesVerified = false;
	// This is true only when the retained setup-time baseline was restored and
	// verified; it must never be inferred from a later oldprot_out value.
	bool originalProtectionVerified = false;
};

struct ByteAccess
{
	using ReadFn = bool (*)(void*, std::uint8_t*, std::size_t) noexcept;
	using WriteFn = WriteOutcome (*)(
		void*, const std::uint8_t*, std::size_t,
		ProtectionBaseline) noexcept;

	void* context = nullptr;
	ReadFn read = nullptr;
	WriteFn write = nullptr;
};

enum class PatchResult : std::uint8_t
{
	Applied,
	Untouched,
	Restored,
	Retained,
	Catastrophic,
};

class PatchState
{
public:
	static constexpr std::size_t kDetourBytes = 5;
	using Bytes = std::array<std::uint8_t, kDetourBytes>;

	void configure(
		const Bytes& original,
		const Bytes& hook,
		ProtectionBaseline baseline
	) noexcept
	{
		original_ = original;
		hook_ = hook;
		baseline_ = baseline;
		configured_ = true;
		installed_ = false;
		freeEligible_ = false;
		catastrophic_ = false;
		writeAttempted_ = false;
		protectionVerified_ = false;
	}

	void clear() noexcept
	{
		original_ = {};
		hook_ = {};
		baseline_ = {};
		configured_ = false;
		installed_ = false;
		freeEligible_ = false;
		catastrophic_ = false;
		writeAttempted_ = false;
		protectionVerified_ = false;
	}

	PatchResult patch(const ByteAccess& access) noexcept
	{
		if (!usable(access) || catastrophic_)
			return PatchResult::Catastrophic;

		Bytes current{};
		if (!read(access, current))
			return markCatastrophic();
		if (same(current, hook_))
		{
			installed_ = true;
			freeEligible_ = false;
			return PatchResult::Applied;
		}
		if (!same(current, original_))
			return markCatastrophic();

		const WriteOutcome outcome = write(
			access, hook_.data());
		if (read(access, current))
		{
			if (same(current, hook_))
			{
				installed_ = true;
				freeEligible_ = false;
				return outcome.writeReportedComplete &&
					outcome.desiredBytesVerified &&
					outcome.originalProtectionVerified
					? PatchResult::Applied
					: PatchResult::Retained;
			}
			if (same(current, original_))
				return classifyOriginal();
		}

		// A failed or unverifiable write gets one checked rollback.  If the
		// target is neither exact original nor exact hook after rollback, it is
		// unsafe to infer which code is executing.
		(void)write(access, original_.data());
		if (read(access, current) && same(current, original_))
		{
			return classifyOriginal();
		}
		if (read(access, current) && same(current, hook_))
		{
			installed_ = true;
			freeEligible_ = false;
			return PatchResult::Retained;
		}
		return markCatastrophic();
	}

	PatchResult restore(const ByteAccess& access) noexcept
	{
		if (!usable(access) || catastrophic_)
			return PatchResult::Catastrophic;

		for (unsigned int attempt = 0; attempt < 3; ++attempt)
		{
			Bytes current{};
			if (!read(access, current))
				return markCatastrophic();
			if (same(current, original_))
			{
				if (!writeAttempted_ || protectionVerified_)
					return writeAttempted_ ? restored() : untouched();
			}
			else if (!same(current, hook_))
				return markCatastrophic();

			(void)write(access, original_.data());
			if (!read(access, current))
				return markCatastrophic();
			if (same(current, original_))
			{
				if (!writeAttempted_ || protectionVerified_)
					return writeAttempted_ ? restored() : untouched();
				continue;
			}
			if (!same(current, hook_))
				return markCatastrophic();
		}

		installed_ = true;
		freeEligible_ = false;
		return PatchResult::Retained;
	}

	bool installed() const noexcept
	{
		return installed_;
	}

	bool freeEligible() const noexcept
	{
		return freeEligible_ && !catastrophic_;
	}

	bool catastrophic() const noexcept
	{
		return catastrophic_;
	}

private:
	bool usable(const ByteAccess& access) const noexcept
	{
		return configured_ && baseline_.valid &&
			access.read != nullptr && access.write != nullptr;
	}

	bool read(const ByteAccess& access, Bytes& bytes) const noexcept
	{
		return access.read(
			access.context, bytes.data(), bytes.size());
	}

	static bool same(const Bytes& left, const Bytes& right) noexcept
	{
		return std::equal(left.begin(), left.end(), right.begin());
	}

	WriteOutcome write(
		const ByteAccess& access,
		const std::uint8_t* bytes
	) noexcept
	{
		const WriteOutcome outcome = access.write(
			access.context, bytes, kDetourBytes, baseline_);
		if (outcome.attempted)
		{
			writeAttempted_ = true;
			protectionVerified_ = outcome.originalProtectionVerified;
		}
		return outcome;
	}

	PatchResult classifyOriginal() noexcept
	{
		if (!writeAttempted_)
			return untouched();
		return protectionVerified_ ? restored() : retained();
	}

	PatchResult untouched() noexcept
	{
		installed_ = false;
		freeEligible_ = true;
		return PatchResult::Untouched;
	}

	PatchResult retained() noexcept
	{
		installed_ = true;
		freeEligible_ = false;
		return PatchResult::Retained;
	}

	PatchResult restored() noexcept
	{
		if (writeAttempted_ && !protectionVerified_)
			return retained();
		installed_ = false;
		freeEligible_ = true;
		return PatchResult::Restored;
	}

	PatchResult markCatastrophic() noexcept
	{
		catastrophic_ = true;
		installed_ = true;
		freeEligible_ = false;
		return PatchResult::Catastrophic;
	}

	Bytes original_{};
	Bytes hook_{};
	ProtectionBaseline baseline_{};
	bool configured_ = false;
	bool installed_ = false;
	bool freeEligible_ = false;
	bool catastrophic_ = false;
	bool writeAttempted_ = false;
	bool protectionVerified_ = false;
};

struct TargetWriteCallbacks
{
	using MakeWritableFn = bool (*)(void*, std::size_t) noexcept;
	using WriteFn = std::size_t (*) (
		void*, const std::uint8_t*, std::size_t) noexcept;
	using ReadFn = bool (*)(void*, std::uint8_t*, std::size_t) noexcept;
	using RestoreBaselineFn = bool (*)(
		void*, ProtectionBaseline) noexcept;

	MakeWritableFn makeWritable = nullptr;
	WriteFn write = nullptr;
	ReadFn read = nullptr;
	RestoreBaselineFn restoreBaseline = nullptr;
};

inline WriteOutcome writeWithRetainedBaseline(
	const TargetWriteCallbacks& callbacks,
	void* context,
	const std::uint8_t* bytes,
	std::size_t size,
	ProtectionBaseline baseline,
	unsigned int retryCount = 3
) noexcept
{
	if (context == nullptr || bytes == nullptr ||
		size != PatchState::kDetourBytes || !baseline.valid ||
		callbacks.makeWritable == nullptr || callbacks.write == nullptr ||
		callbacks.read == nullptr || callbacks.restoreBaseline == nullptr ||
		retryCount == 0)
	{
		return WriteOutcome{};
	}

	WriteOutcome outcome{};
	std::array<std::uint8_t, PatchState::kDetourBytes> observed{};
	for (unsigned int attempt = 0; attempt < retryCount; ++attempt)
	{
		if (!callbacks.makeWritable(context, size))
		{
			if (outcome.attempted)
				return outcome;
			continue;
		}
		outcome.attempted = true;

		const bool wrote = callbacks.write(context, bytes, size) == size;
		const bool readBack = callbacks.read(
			context, observed.data(), observed.size()) &&
			std::equal(observed.begin(), observed.end(), bytes);
		const bool restored = callbacks.restoreBaseline(context, baseline);
		outcome.writeReportedComplete = wrote;
		outcome.desiredBytesVerified = readBack;
		outcome.originalProtectionVerified = restored;
		if (restored)
			return outcome;
	}

	return outcome;
}

enum class LifecyclePhase : std::uint8_t
{
	Fresh,
	Installing,
	Provisional,
	Active,
	Disabled,
	DisabledHooked,
	Catastrophic,
};

class Lifecycle
{
public:
	Lifecycle() noexcept = default;

	bool beginInstall() noexcept
	{
		if (installAttempted_)
			return false;
		installAttempted_ = true;
		phase_ = LifecyclePhase::Installing;
		return true;
	}

	void failBeforeHook() noexcept
	{
		managed_.store(false, std::memory_order_seq_cst);
		if (!hookInstalled_)
			phase_ = LifecyclePhase::Disabled;
	}

	void markProvisional(bool originalCallable) noexcept
	{
		phase_ = LifecyclePhase::Provisional;
		hookInstalled_ = true;
		originalCallable_ = originalCallable;
		managed_.store(false, std::memory_order_seq_cst);
	}

	bool activate() noexcept
	{
		if (phase_ != LifecyclePhase::Provisional ||
			!hookInstalled_ || !originalCallable_)
		{
			return false;
		}
		phase_ = LifecyclePhase::Active;
		managed_.store(true, std::memory_order_seq_cst);
		return true;
	}

	void disableManaged() noexcept
	{
		managed_.store(false, std::memory_order_seq_cst);
	}

	bool managed() const noexcept
	{
		return managed_.load(std::memory_order_seq_cst);
	}

	bool canRebind() const noexcept
	{
		return phase_ == LifecyclePhase::Active && managed();
	}

	bool hookInstalled() const noexcept
	{
		return hookInstalled_;
	}

	bool originalCallable() const noexcept
	{
		return originalCallable_;
	}

	LifecyclePhase phase() const noexcept
	{
		return phase_;
	}

	bool installAttempted() const noexcept
	{
		return installAttempted_;
	}

	bool finishUnhook(bool success) noexcept
	{
		managed_.store(false, std::memory_order_seq_cst);
		if (!hookInstalled_)
			return true;
		if (phase_ == LifecyclePhase::Catastrophic)
			return false;

		if (!success)
		{
			phase_ = LifecyclePhase::DisabledHooked;
			return false;
		}

		phase_ = LifecyclePhase::Disabled;
		hookInstalled_ = false;
		originalCallable_ = false;
		return true;
	}

	void markCatastrophic() noexcept
	{
		managed_.store(false, std::memory_order_seq_cst);
		phase_ = LifecyclePhase::Catastrophic;
		hookInstalled_ = true;
	}

	bool catastrophic() const noexcept
	{
		return phase_ == LifecyclePhase::Catastrophic;
	}

	void enterReader() noexcept
	{
		readers_.fetch_add(1, std::memory_order_seq_cst);
	}

	void leaveReader() noexcept
	{
		readers_.fetch_sub(1, std::memory_order_seq_cst);
	}

	bool readersDrained() const noexcept
	{
		return readers_.load(std::memory_order_seq_cst) == 0;
	}

	bool quiescentForUnhook() const noexcept
	{
		return hookInstalled_ && !managed() && readersDrained();
	}

private:
	LifecyclePhase phase_ = LifecyclePhase::Fresh;
	bool installAttempted_ = false;
	bool hookInstalled_ = false;
	bool originalCallable_ = false;
	std::atomic<bool> managed_{false};
	std::atomic<std::uint32_t> readers_{0};
};

static_assert(std::atomic<bool>::is_always_lock_free,
	"managed hook state must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
	"hook reader counter must be lock-free");

namespace detail
{
template <typename Fn>
decltype(auto) withLifecycleLock(std::mutex& mutex, Fn&& fn)
{
	std::lock_guard<std::mutex> lock(mutex);
	return std::forward<Fn>(fn)();
}
}

inline void* guard(
	void* data,
	std::uint32_t appId,
	bool create,
	HotReloadState::Store::ReadHandle& read,
	HotReloadState::Store::ReadHandle& authoritativeRead,
	const AppDataLayout::Layout& layout
) noexcept
{
	if (data == nullptr || create || !read.contains(appId))
		return data;

	const auto* const bytes = static_cast<const std::uint8_t*>(data);
	bool shaEmpty = true;
	for (std::size_t index = 0;
		index < AppDataLayout::detail::kShaBytes; ++index)
	{
		if (bytes[layout.shaOffset + index] != 0)
		{
			shaEmpty = false;
			break;
		}
	}

	const bool skipSet = bytes[layout.skipOffset] != 0;
	const bool authoritative = authoritativeRead.contains(appId);
	switch (AppInfoStatePolicy::decide(
		true, authoritative, false, shaEmpty, skipSet))
	{
		case AppInfoStatePolicy::Action::MarkSkip:
			static_cast<std::uint8_t*>(data)[layout.skipOffset] = 1;
			break;
		case AppInfoStatePolicy::Action::SignalResolved:
			read.noteResolved();
			break;
		case AppInfoStatePolicy::Action::MarkSkipAndSignalResolved:
			static_cast<std::uint8_t*>(data)[layout.skipOffset] = 1;
			read.noteResolved();
			break;
		case AppInfoStatePolicy::Action::None:
			break;
	}
	return data;
}

inline void* guard(
	void* data,
	std::uint32_t appId,
	bool create,
	HotReloadState::Store::ReadHandle& read,
	const AppDataLayout::Layout& layout
) noexcept
{
	HotReloadState::Store empty;
	auto authoritativeRead = empty.readHandle();
	return guard(data, appId, create, read, authoritativeRead, layout);
}

bool setup(HotReloadState::Store& store) noexcept;
void remove() noexcept;
bool ready() noexcept;
bool catastrophic() noexcept;
bool resolvedDirtyHint() noexcept;
bool takeResolvedDirty() noexcept;

void publishAuthoritative(
	const std::unordered_set<std::uint32_t>& appIds) noexcept;
bool isAuthoritative(std::uint32_t appId) noexcept;

// Re-read Steam's appinfo.vdf through the captured CAppInfoCache instance and
// signal managed apps whose CAppData now carries a real SHA.  This is called
// only by the existing async provisioning worker; unavailable optional
// locators leave the normal restart path untouched.
AppInfoReload::Result reloadFromDisk(
	std::span<const std::uint32_t> appIds) noexcept;
}
