// Regression tests for the provisioning scheduling policy
// (src/feats/provision_schedule.hpp).

#include "feats/provision_schedule.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

int main()
{
    using AppInfoProvision::CacheReadiness;
    using AppInfoProvision::asyncProvisionEnabled;
    using AppInfoProvision::shouldMarkColdCacheSanitized;
    using AppInfoProvision::PreinitProvisionAction;
    using AppInfoProvision::preinitProvisionAction;
    using AppInfoProvision::RefreshReason;
    using AppInfoProvision::RefreshRequest;
    using AppInfoProvision::reasonMask;
    using AppInfoProvision::selectRuntimePublishCandidates;

    check(preinitProvisionAction(true) ==
              PreinitProvisionAction::ColdFallbackThenSplice,
          "async setup provisions only missing apps before the appinfo splice");
    check(preinitProvisionAction(false) ==
              PreinitProvisionAction::SynchronousProvision,
          "disabled async mode keeps the full synchronous setup pass");

    check(shouldMarkColdCacheSanitized(false),
          "provider-normalized cold results suppress raw PICS replacement");
    check(!shouldMarkColdCacheSanitized(true),
          "explicit fallback stays replaceable until cache provenance is checked");

    check(asyncProvisionEnabled(true, nullptr),
          "config enables async provisioning by default");
    check(!asyncProvisionEnabled(true, "0"),
          "environment zero disables async provisioning");
    check(!asyncProvisionEnabled(true, "false"),
          "environment false disables async provisioning");
    check(asyncProvisionEnabled(false, "1"),
          "environment one can re-enable async provisioning");
    check(!asyncProvisionEnabled(true, "unexpected"),
          "invalid environment value fails closed");

    const std::unordered_set<std::uint32_t> managed{420530, 555, 666, 777, 888};
    const std::unordered_set<std::uint32_t> synthetic{420530, 999};
    const std::unordered_map<std::uint32_t, std::uint64_t> generations{
        {420530, 1}, {555, 1}, {666, 2}, {777, 1}, {888, 1}, {999, 1}};
    check(selectRuntimePublishCandidates(
              std::vector<RefreshRequest>{
                  {999, 0, 1, reasonMask(RefreshReason::PicsChanges), true, true},
                  {420530, 0, 1, reasonMask(RefreshReason::PicsChanges), true, true},
                  {555, 0, 1, reasonMask(RefreshReason::PicsChanges), true, true},
                  {777, 0, 1, reasonMask(RefreshReason::HotAdd), true, true},
                  {888, 0, 1, reasonMask(RefreshReason::LocalInputs), true, true},
                  {666, 0, 1, reasonMask(RefreshReason::HotAdd), true, true},
                  {777, 0, 1, reasonMask(RefreshReason::HotAdd), true, false},
              }, managed, synthetic, generations) ==
              std::vector<std::uint32_t>({777, 888, 420530}),
          "runtime publication admits current local updates and suppressed synthetic PICS only");
    if (failures != 0)
    {
        std::fprintf(stderr, "test_provision_schedule: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("provision schedule tests passed");
    return 0;
}
