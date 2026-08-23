// SPDX-License-Identifier: AGPL-3.0-only
//
// Bounded cross-thread queue for Steam-UI ownership presentation changes.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace LibraryRemovalPolicy
{
inline constexpr std::uint32_t hiddenOwnershipFlags(
	std::uint32_t) noexcept
{
	return 0;
}

inline std::optional<std::size_t> deriveOwnershipOffset(
	std::span<const std::uint8_t> instruction) noexcept
{
	// `8B /r` with mod=01 (disp8) and reg=eax, i.e. `mov eax,[base+disp8]`.
	// Steam's own register allocation for the app pointer is not stable across
	// builds (ecx on 2026-08-03, edx on 2026-08-16), so accept either base while
	// still refusing any other destination register, a SIB byte (rm=100), and
	// [ebp+disp8] (rm=101), none of which are this read.
	if (instruction.size() < 3 || instruction[0] != 0x8B ||
		(instruction[1] != 0x41 && instruction[1] != 0x42))
	{
		return std::nullopt;
	}
	const std::size_t offset = instruction[2];
	if (offset == 0 || offset > 0x100 || (offset % alignof(std::uint32_t)) != 0)
		return std::nullopt;
	return offset;
}

enum class Action : std::uint8_t
{
	Remove,
	Restore,
};

struct Work
{
	std::uint32_t appId = 0;
	Action action = Action::Remove;

	bool operator==(const Work&) const = default;
};

class Queue
{
public:
	explicit Queue(std::size_t capacity)
		: capacity_(capacity) {}

	bool push(std::uint32_t appId)
	{
		if (appId == 0)
			return false;

		std::lock_guard<std::mutex> lock(mutex_);
		if (closed_)
			return false;

		const bool newlyDesired = desired_.insert(appId).second;
		if (newlyDesired && desired_.size() > capacity_)
		{
			desired_.erase(appId);
			return false;
		}
		reconcileLocked(appId);
		return newlyDesired;
	}

	void cancel(std::uint32_t appId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		desired_.erase(appId);
		reconcileLocked(appId);
	}

	// Called only after package 0 and the license refresh have accepted the
	// generation that re-added this id. This separates immediate cancellation
	// of a stale removal from the later, correctly ordered UI restoration.
	void readyToRestore(std::uint32_t appId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (closed_ || appId == 0 || desired_.count(appId) != 0)
			return;
		presentReady_.insert(appId);
		reconcileLocked(appId);
	}

	std::optional<Work> drainOne()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (closed_ || inFlight_.has_value())
			return std::nullopt;

		if (reassertRequested_)
		{
			std::vector<std::uint32_t> sorted(effectiveRemoved_.begin(),
			                                  effectiveRemoved_.end());
			std::sort(sorted.begin(), sorted.end());
			for (const std::uint32_t appId : sorted)
			{
				if (desired_.count(appId) != 0)
					enqueueLocked({appId, Action::Remove});
			}
			reassertRequested_ = false;
		}

		while (!pending_.empty())
		{
			const Work work = pending_.front();
			pending_.pop_front();
			pendingSetLocked(work.action).erase(work.appId);
			const bool current = work.action == Action::Remove
				? desired_.count(work.appId) != 0
				: desired_.count(work.appId) == 0 &&
				  presentReady_.count(work.appId) != 0;
			if (!current)
				continue;
			inFlight_ = work;
			return work;
		}
		return std::nullopt;
	}

	// Publish the effect before MarkAppChange: that call may synchronously enter
	// the complete-change hook, which must observe the new effective state.
	bool begin(const Work& work)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (closed_ || !inFlight_ || *inFlight_ != work)
			return false;
		const bool current = work.action == Action::Remove
			? desired_.count(work.appId) != 0
			: desired_.count(work.appId) == 0 &&
			  presentReady_.count(work.appId) != 0;
		if (!current)
		{
			inFlight_.reset();
			reconcileLocked(work.appId);
			return false;
		}
		if (work.action == Action::Remove)
			effectiveRemoved_.insert(work.appId);
		else
			effectiveRemoved_.erase(work.appId);
		return true;
	}

	void finish(const Work& work)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (inFlight_ && *inFlight_ == work)
			inFlight_.reset();
		if (work.action == Action::Restore && desired_.count(work.appId) == 0)
			presentReady_.erase(work.appId);
		reconcileLocked(work.appId);
	}

	void retry(const Work& work)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (inFlight_ && *inFlight_ == work)
			inFlight_.reset();
		reconcileLocked(work.appId);
	}

	std::vector<std::uint32_t> appliedSnapshot() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<std::uint32_t> result;
		result.reserve(effectiveRemoved_.size());
		for (const std::uint32_t appId : effectiveRemoved_)
		{
			if (!closed_ && desired_.count(appId) != 0)
				result.push_back(appId);
		}
		std::sort(result.begin(), result.end());
		return result;
	}

	bool desiredRemoved(std::uint32_t appId) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return !closed_ && desired_.count(appId) != 0;
	}

	void requestFullReassert()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!closed_)
			reassertRequested_ = true;
	}

	void close()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		closed_ = true;
		reassertRequested_ = false;
		pending_.clear();
		pendingRemovals_.clear();
		pendingRestores_.clear();
		desired_.clear();
		effectiveRemoved_.clear();
		presentReady_.clear();
		inFlight_.reset();
	}

	void reopen()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		closed_ = false;
		reassertRequested_ = false;
		pending_.clear();
		pendingRemovals_.clear();
		pendingRestores_.clear();
		desired_.clear();
		effectiveRemoved_.clear();
		presentReady_.clear();
		inFlight_.reset();
	}

private:
	std::unordered_set<std::uint32_t>& pendingSetLocked(Action action)
	{
		return action == Action::Remove ? pendingRemovals_ : pendingRestores_;
	}

	void erasePendingLocked(std::uint32_t appId, Action action)
	{
		auto& set = pendingSetLocked(action);
		if (set.erase(appId) == 0)
			return;
		pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
			[appId, action](const Work& work)
			{
				return work.appId == appId && work.action == action;
			}), pending_.end());
	}

	bool enqueueLocked(Work work)
	{
		auto& set = pendingSetLocked(work.action);
		if (set.count(work.appId) != 0 ||
			(inFlight_ && *inFlight_ == work))
		{
			return false;
		}
		pending_.push_back(work);
		set.insert(work.appId);
		return true;
	}

	void reconcileLocked(std::uint32_t appId)
	{
		const bool removeInFlight = inFlight_ &&
			inFlight_->appId == appId && inFlight_->action == Action::Remove;
		const bool restoreInFlight = inFlight_ &&
			inFlight_->appId == appId && inFlight_->action == Action::Restore;
		if (desired_.count(appId) != 0)
		{
			presentReady_.erase(appId);
			erasePendingLocked(appId, Action::Restore);
			if (effectiveRemoved_.count(appId) == 0 || restoreInFlight)
				enqueueLocked({appId, Action::Remove});
			return;
		}

		erasePendingLocked(appId, Action::Remove);
		if (presentReady_.count(appId) != 0 &&
			(effectiveRemoved_.count(appId) != 0 || removeInFlight))
		{
			enqueueLocked({appId, Action::Restore});
		}
	}

	const std::size_t capacity_;
	mutable std::mutex mutex_;
	bool closed_ = false;
	bool reassertRequested_ = false;
	std::deque<Work> pending_;
	std::unordered_set<std::uint32_t> pendingRemovals_;
	std::unordered_set<std::uint32_t> pendingRestores_;
	std::unordered_set<std::uint32_t> desired_;
	std::unordered_set<std::uint32_t> effectiveRemoved_;
	std::unordered_set<std::uint32_t> presentReady_;
	std::optional<Work> inFlight_;
};
} // namespace LibraryRemovalPolicy
