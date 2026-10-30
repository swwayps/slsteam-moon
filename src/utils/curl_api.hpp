#pragma once

#include "../utils.hpp"

#include <curl/curl.h>
#include <dlfcn.h>
#include <exception>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

// libcurl entry points, resolved at run time: SLSsteam does not link libcurl,
// and a dlopen from this audit namespace gets the Steam runtime's copy.
struct CurlApi
{
	decltype(&curl_easy_init) easy_init;
	decltype(&curl_easy_setopt) easy_setopt;
	decltype(&curl_easy_perform) easy_perform;
	decltype(&curl_easy_cleanup) easy_cleanup;
	decltype(&curl_easy_getinfo) easy_getinfo;
	decltype(&curl_easy_strerror) easy_strerror;
	decltype(&curl_easy_send) easy_send;
	decltype(&curl_easy_recv) easy_recv;
	decltype(&curl_slist_append) slist_append;
	decltype(&curl_slist_free_all) slist_free_all;
};

// Every entry point, or nullptr when libcurl or one of them is missing.
// Resolved once, on the first call; safe from any thread.
inline const CurlApi* curlApi()
{
	static const CurlApi* const api = []() -> const CurlApi* {
		void* lib = dlopen("libcurl.so.4", RTLD_NOLOAD | RTLD_LAZY);
		if (!lib) lib = dlopen("libcurl.so.4", RTLD_LAZY);
		if (!lib) lib = RTLD_DEFAULT;
		static CurlApi resolved {};
		const auto bind = [lib](auto& fn, const char* name) {
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
			return fn != nullptr;
		};
		const bool complete = bind(resolved.easy_init, "curl_easy_init")
			&& bind(resolved.easy_setopt, "curl_easy_setopt")
			&& bind(resolved.easy_perform, "curl_easy_perform")
			&& bind(resolved.easy_cleanup, "curl_easy_cleanup")
			&& bind(resolved.easy_getinfo, "curl_easy_getinfo")
			&& bind(resolved.easy_strerror, "curl_easy_strerror")
			&& bind(resolved.easy_send, "curl_easy_send")
			&& bind(resolved.easy_recv, "curl_easy_recv")
			&& bind(resolved.slist_append, "curl_slist_append")
			&& bind(resolved.slist_free_all, "curl_slist_free_all");
		return complete ? &resolved : nullptr;
	}();
	return api;
}

// glibc < 2.43 crashes in the thread-local storage of libraries an audit
// module loaded; a thread started for the transfer is not affected, so on
// those versions `transfer` runs on a fresh thread and the caller waits for
// it, as upstream dev-X86_64 056dfc3 does for its curl calls. Everything that
// uses one libcurl handle must stay inside one `transfer`. When no thread can
// be started the transfer runs here, as it did before.
template <typename Transfer>
auto runCurlTransfer(Transfer&& transfer) -> decltype(transfer())
{
	if (!Utils::isGLIBCBugged())
		return transfer();
	using Result = decltype(transfer());
	std::optional<Result> result;
	std::exception_ptr error;
	try
	{
		std::thread worker([&]() noexcept {
			try { result.emplace(transfer()); }
			catch (...) { error = std::current_exception(); }
		});
		worker.join();
	}
	catch (const std::system_error&)
	{
		return transfer();
	}
	if (error) std::rethrow_exception(error);
	return std::move(*result);
}
