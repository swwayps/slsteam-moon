// libcurl stand-in for tests: tools/fake_libcurl.cpp builds a libcurl.so.4
// whose entry points forward to the FakeCurl a test installs in g_fakeCurl.
#pragma once
#include <curl/curl.h>
#include <cstddef>

struct FakeCurl
{
	virtual CURL* init() = 0;
	virtual CURLcode setopt(CURL* handle, CURLoption option, long number,
	                        void* pointer) = 0;
	virtual CURLcode perform(CURL* handle) = 0;
	virtual void cleanup(CURL* handle) = 0;
	virtual CURLcode getinfo(CURL* handle, CURLINFO info, void* out) = 0;
	virtual CURLcode send(CURL* handle, const void* data, std::size_t length,
	                      std::size_t* sent) = 0;
	virtual CURLcode recv(CURL* handle, void* buffer, std::size_t length,
	                      std::size_t* received) = 0;

protected:
	~FakeCurl() = default;
};

extern FakeCurl* g_fakeCurl;
