// Build: g++ -m32 -shared -fPIC -Wl,-soname,libcurl.so.4 tools/fake_libcurl.cpp
// A test links this instead of libcurl, so curlApi() resolves these.
#include "fake_libcurl.hpp"

#include <cstdarg>

FakeCurl* g_fakeCurl = nullptr;

extern "C" {

CURL* curl_easy_init() { return g_fakeCurl->init(); }

// Parenthesized names: curl.h defines both as function-like macros.
CURLcode (curl_easy_setopt)(CURL* handle, CURLoption option, ...)
{
	va_list args;
	va_start(args, option);
	long number = 0;
	void* pointer = nullptr;
	if (option < CURLOPTTYPE_OBJECTPOINT) number = va_arg(args, long);
	else if (option < CURLOPTTYPE_OFF_T) pointer = va_arg(args, void*);
	va_end(args);
	return g_fakeCurl->setopt(handle, option, number, pointer);
}

CURLcode curl_easy_perform(CURL* handle) { return g_fakeCurl->perform(handle); }

void curl_easy_cleanup(CURL* handle) { g_fakeCurl->cleanup(handle); }

CURLcode (curl_easy_getinfo)(CURL* handle, CURLINFO info, ...)
{
	va_list args;
	va_start(args, info);
	void* out = va_arg(args, void*);
	va_end(args);
	return g_fakeCurl->getinfo(handle, info, out);
}

const char* curl_easy_strerror(CURLcode) { return "fake libcurl"; }

CURLcode curl_easy_send(CURL* handle, const void* data, size_t length, size_t* sent)
{
	return g_fakeCurl->send(handle, data, length, sent);
}

CURLcode curl_easy_recv(CURL* handle, void* buffer, size_t length, size_t* received)
{
	return g_fakeCurl->recv(handle, buffer, length, received);
}

struct curl_slist* curl_slist_append(struct curl_slist* list, const char*) { return list; }

void curl_slist_free_all(struct curl_slist*) {}

} // extern "C"
