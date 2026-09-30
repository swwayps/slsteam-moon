#pragma once

#include <string>
#include <vector>


namespace Curl
{
	constexpr int DEFAULT_TIMEOUT = 15;

	int downloadString(const char* url, std::string& out, const int timeOut = DEFAULT_TIMEOUT);
	int downloadString(const char* url, const std::vector<std::string>& headers, std::string& out, int timeOut = DEFAULT_TIMEOUT);
}
