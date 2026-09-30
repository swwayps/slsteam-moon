#include "../src/feats/hotreload_capabilities.hpp"
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
HotReloadCapabilities::Matrix complete()
{
	return { true, true, true, true };
}
}
int main()
{
	check(complete().canRemoveVisually(),
	      "complete matrix enables live visual removal");
	auto withoutRunFrame = complete();
	withoutRunFrame.uiRunFrame = false;
	check(!withoutRunFrame.canRemoveVisually(),
	      "missing UI run frame disables visual removal");
	auto withoutGetApp = complete();
	withoutGetApp.uiGetAppById = false;
	check(!withoutGetApp.canRemoveVisually(),
	      "missing app lookup disables visual removal");
	auto withoutMarkChange = complete();
	withoutMarkChange.uiMarkAppChange = false;
	check(!withoutMarkChange.canRemoveVisually(),
	      "missing app change mark disables visual removal");
	auto withoutOwnershipLayout = complete();
	withoutOwnershipLayout.uiOwnershipLayout = false;
	check(!withoutOwnershipLayout.canRemoveVisually(),
	      "invalid derived ownership layout disables visual removal");
	return failures == 0 ? 0 : 1;
}
