// Utils::tryConvertToNumber, which parses API command arguments on the watcher
// thread. Build and run: make test-utils-number
#include "utils.hpp"

#include <cstdint>
#include <cstdio>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}

template<typename T>
bool convertWithoutThrowing(const char* str, T& out, bool& converted)
{
	try
	{
		converted = Utils::tryConvertToNumber(str, out);
		return true;
	}
	catch (...)
	{
		return false;
	}
}
}

int main()
{
	uint32_t u = 7;
	bool converted = false;
	check(convertWithoutThrowing("480", u, converted) && converted && u == 480,
	      "a decimal id converts");
	check(convertWithoutThrowing("4294967295", u, converted) && converted && u == 4294967295u,
	      "the largest uint32 converts");

	u = 7;
	check(convertWithoutThrowing("4294967296", u, converted), "an id past uint32 does not throw");
	check(!converted && u == 7, "an id past uint32 is rejected and leaves the output alone");

	check(convertWithoutThrowing("48a0", u, converted) && !converted, "trailing letters are rejected");
	check(convertWithoutThrowing("", u, converted) && !converted, "an empty argument is rejected");

	int32_t i = 7;
	check(convertWithoutThrowing("2147483647", i, converted) && converted && i == 2147483647,
	      "the largest int32 converts");
	i = 7;
	check(convertWithoutThrowing("2147483648", i, converted) && !converted && i == 7,
	      "a value past int32 is rejected without throwing");

	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
