// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <curl/curl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// A libcurl body sink that keeps at most `limit` bytes. The write callback runs
// inside libcurl's C frames, so it never throws: past the limit, or when the
// string cannot grow, it returns 0 and libcurl ends the transfer with
// CURLE_WRITE_ERROR.
struct CurlBodySink
{
	std::string* out;
	std::size_t limit;
};

inline std::size_t curlBodySinkWrite(const char* data, std::size_t size,
                                     std::size_t count, CurlBodySink* sink) noexcept
{
	if (size && count > SIZE_MAX / size) return 0;
	const std::size_t bytes = size * count;
	if (sink->out->size() > sink->limit || bytes > sink->limit - sink->out->size())
		return 0;
	try
	{
		sink->out->append(data, bytes);
	}
	catch (...)
	{
		return 0;
	}
	return bytes;
}

// CURLOPT_REDIR_PROTOCOLS for `url`: a redirect never takes an https request
// to plain http.
inline long curlRedirectProtocols(std::string_view url) noexcept
{
	return url.substr(0, 8) == "https://"
		? static_cast<long>(CURLPROTO_HTTPS)
		: static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS);
}
