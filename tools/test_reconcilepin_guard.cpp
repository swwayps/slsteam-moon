// The ReconcilePin hooks run their depot patch through runPatched: a patch
// that throws does not unwind into Steam's content thread, and the failure is
// noted once per throw.
// Build (from repo root):
//   g++ -std=c++20 -Wall -Wextra -Wpedantic -I src tools/test_reconcilepin_guard.cpp -o /tmp/test_reconcilepin_guard && /tmp/test_reconcilepin_guard
#include "feats/reconcilepin_guard.hpp"
#include <cstdio>
#include <stdexcept>
namespace
{
int failures = 0;
void expect(bool condition, const char* message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message);
	if (!condition) ++failures;
}
}
int main()
{
	int notes = 0;
	bool escaped = false;
	try
	{
		ReconcilePin::detail::runPatched([] { throw std::runtime_error("patch failed"); },
		                                 [&] { ++notes; });
	}
	catch (...)
	{
		escaped = true;
	}
	expect(!escaped, "a patch that throws does not unwind into the hook");
	expect(notes == 1, "the failure is noted once");

	int patched = 0;
	notes = 0;
	ReconcilePin::detail::runPatched([&] { ++patched; }, [&] { ++notes; });
	expect(patched == 1 && notes == 0, "a patch that succeeds runs once and notes nothing");

	if (failures == 0) std::puts("all ReconcilePin guard checks passed");
	return failures == 0 ? 0 : 1;
}
