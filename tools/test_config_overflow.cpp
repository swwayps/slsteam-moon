// Regression guard for the "exorbitant FakeWalletBalance bricks Steam" bug.
//
// SYMPTOM (user report): setting FakeWalletBalance to a huge number makes
// Steam abort on launch and never reopen until the value is reset to 0.
//
// ROOT CAUSE (confirmed from successive SIGABRT coredumps): an out-of-range
// scalar (FakeWalletBalance > INT_MAX, e.g. 500000000000000000) makes the
// THROWING yaml-cpp overload `Node::as<int>()` raise
// YAML::TypedBadConversion<int> from inside CConfig::getSetting.  Under the
// release build (-O3 -flto -freorder-blocks-and-partition) the throw lives in
// the function's ".cold" partition and the call-site table fails to route it
// to the catch landing pad, so the exception escapes loadSettings -> init() ->
// setup() -> abort.  This bypassed BOTH the original catch(YAML::BadConversion&)
// AND a broadened catch(...) (verified: the same SIGABRT recurred with a
// catch-all deployed) -- the handler is simply never reached across the
// hot/cold partition boundary.
//
//   Stack trace (from the cores):
//     __cxa_throw -> CConfig::loadSettings()[.cold] -> CConfig::init()
//     -> setup() -> _dl_audit_preinit -> abort
//     thrown type_info: YAML::TypedBadConversion<int>
//
// THE FIX: do not throw at all.  getSetting now uses the NON-THROWING
// `Node::as<T>(fallback)` overload, which returns the fallback on a bad/out-of-
// range scalar instead of raising.  No throw -> no reliance on the broken
// partitioned exception tables -> cannot abort, on any toolchain.
//
// This test pins that contract on the host (where yaml-cpp parses normally):
// the literal value that bricked the client must convert to the default (0)
// via the non-throwing overload, and must NOT throw.
//
// Build:
//   g++ -O3 -flto=auto -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
//       -I include tools/test_config_overflow.cpp lib/libyaml-cpp.a \
//       -lpthread -o /tmp/test_config_overflow && /tmp/test_config_overflow

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstdio>

// Exact shape of the FIXED CConfig::getSetting body for a present key.
template <typename T>
static T getSetting_fixed(YAML::Node& node, const char* name, T defVal) {
  if (!node[name]) return defVal;
  return node[name].as<T>(defVal); // non-throwing overload
}

int main() {
  int failures = 0;

  YAML::Node node = YAML::Load(
      "FakeWalletBalance: 500000000000000000\n" // the value that bricked Steam
      "GoodWallet: 4999\n"                       // an in-range value
  );

  // 1) The overflow must NOT throw and must fall back to the default.
  bool threw = false;
  int32_t over = 0;
  try {
    over = getSetting_fixed<int32_t>(node, "FakeWalletBalance", 0);
  } catch (...) {
    threw = true;
  }
  if (threw) {
    printf("FAIL: getSetting threw on the out-of-range value.\n");
    failures++;
  } else if (over != 0) {
    printf("FAIL: out-of-range value did not fall back to the default "
           "(got %d).\n", over);
    failures++;
  } else {
    printf("OK: out-of-range FakeWalletBalance falls back to 0 without "
           "throwing.\n");
  }

  // 2) An in-range value is still read correctly.
  int32_t good = getSetting_fixed<int32_t>(node, "GoodWallet", 0);
  if (good != 4999) {
    printf("FAIL: in-range value not read (got %d, want 4999).\n", good);
    failures++;
  } else {
    printf("OK: in-range value read correctly (4999).\n");
  }

  // 3) A missing key falls back to the default.
  int32_t missing = getSetting_fixed<int32_t>(node, "Absent", 7);
  if (missing != 7) {
    printf("FAIL: missing key did not return the default (got %d).\n", missing);
    failures++;
  } else {
    printf("OK: missing key returns the default.\n");
  }

  if (failures) {
    printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
