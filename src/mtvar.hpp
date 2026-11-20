#pragma once

#include <mutex>
#include <shared_mutex>


template<typename T> class MTVariable
{
private:
	T instance;
	std::shared_mutex mutex;

public:
	MTVariable() : instance(empty()) { }
	MTVariable(T instance) : instance(instance) { }

	//Returns default instance
	T empty()
	{
		return T();
	}

	T get()
	{
		const auto lock = std::shared_lock(mutex);
		//Return copy
		return T(instance);
	}

	// Upstream's name for a by-value read. Upstream's get() returns a shared
	// snapshot pointer instead; here every get() is already a copy.
	T copy()
	{
		return get();
	}

	template<typename Key>
	bool contains(const Key& key)
	{
		const auto lock = std::shared_lock(mutex);
		return instance.contains(key);
	}
	// Runs fn on the value in place under the shared lock and returns its
	// result; fn must not use this variable again.
	template<typename Fn>
	auto read(Fn&& fn)
	{
		const auto lock = std::shared_lock(mutex);
		return fn(static_cast<const T&>(instance));
	}

	void set(T value)
	{
		const auto lock = std::lock_guard(mutex);
		instance = value;
	}

	void operator=(MTVariable<T> other)
	{
		set(other.instance);
	}
};
