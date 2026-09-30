#include "../src/feats/license_refresh_policy.hpp"

#include <iostream>
#include <string_view>

namespace
{
int failures = 0;

void check(bool condition, std::string_view message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}
}

int main()
{
	using LicenseRefreshPolicy::Action;
	using LicenseRefreshPolicy::decide;
	using LicenseRefreshPolicy::unresolvedStateSafe;

	check(decide(true, true, true, true) == Action::Process,
		"complete capability processes change");
	check(decide(true, true, false, true) == Action::Defer,
		"missing unresolved-app guard defers an unsafe change");
	check(decide(false, true, true, true) == Action::Defer,
		"missing mark function leaves the change restart-recoverable");
	check(decide(true, false, true, true) == Action::Defer,
		"missing process function leaves the change restart-recoverable");
	check(decide(true, true, true, false) == Action::NoChange,
		"unchanged state emits no duplicate refresh");
	check(decide(false, false, false, false) == Action::NoChange,
		"an unchanged state does not require runtime capabilities");
	check(unresolvedStateSafe(false, false),
		"removal-only changes do not require the unresolved-app guard");
	check(!unresolvedStateSafe(true, false),
		"a newly unresolved addition requires the guard");
	check(unresolvedStateSafe(true, true),
		"the installed guard makes an unresolved addition safe to process");

	return failures == 0 ? 0 : 1;
}
