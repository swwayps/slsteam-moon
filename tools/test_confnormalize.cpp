// Regression guard for the "malformed AdditionalApps indentation bricks Steam"
// bug (config_parse_abort_analysis.md).
//
// SYMPTOM: LuaTools adds a game; the SLSsteam config.yaml ends up with a block
// sequence whose items have INCONSISTENT indentation, e.g.
//
//     AdditionalApps:
//       - 4149320        <- inserted at 2-space indent
//     - 381210           <- pre-existing 0-space entries
//     - 489980
//
// yaml-cpp rejects this whole document with YAML::ParserException
// ("end of map not found"). In the release build the throw can escape the
// catch and abort the client at startup -> Steam boot loop. Even when it does
// not abort, the whole config is lost -> every added game reads as unmanaged.
//
// THE FIX (this unit): a pure, last-resort text repair that normalises the
// indentation of block-sequence items so the document parses again, WITHOUT
// touching a file that is already well-formed. It runs only after yaml-cpp has
// already rejected the file, and the caller re-validates the result, so it can
// never make a parseable file worse.
//
// Build:
//   g++ -O3 -flto=auto -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
//       -I include -I src tools/test_confnormalize.cpp lib/libyaml-cpp.a \
//       -lpthread -o /tmp/test_confnormalize && /tmp/test_confnormalize

#include "confnormalize.hpp"
#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(bool cond, const char* msg) {
  if (cond) {
    printf("OK:   %s\n", msg);
  } else {
    printf("FAIL: %s\n", msg);
    failures++;
  }
}

// Does yaml-cpp accept this text?
static bool parses(const std::string& text) {
  try {
    YAML::Load(text);
    return true;
  } catch (...) {
    return false;
  }
}

int main() {
  // 1) The exact corruption from the field report: mixed 2-space / 0-space
  //    items under AdditionalApps. Must become parseable, must report a change,
  //    and must PRESERVE every id (including the newly-added 4149320).
  {
    const std::string bad =
        "API: false\n"
        "AdditionalApps:\n"
        "  - 4149320   # added via LuaTools\n"
        "- 381210\n"
        "- 489980\n"
        "AppIds: null\n"
        "LogLevel: 2\n";

    check(!parses(bad), "precondition: the malformed config does NOT parse");

    bool changed = false;
    const std::string fixed = ConfNormalize::repairSeqIndent(bad, &changed);

    check(changed, "repair: reports it changed the malformed config");
    check(parses(fixed), "repair: the result parses");

    if (parses(fixed)) {
      YAML::Node n = YAML::Load(fixed);
      auto apps = n["AdditionalApps"];
      check(apps.IsSequence() && apps.size() == 3,
            "repair: all three ids survive the repair");
      bool has4149320 = false, has381210 = false, has489980 = false;
      for (auto e : apps) {
        const auto v = e.as<long long>(-1);
        if (v == 4149320) has4149320 = true;
        if (v == 381210) has381210 = true;
        if (v == 489980) has489980 = true;
      }
      check(has4149320, "repair: the newly-added id (4149320) is preserved");
      check(has381210 && has489980, "repair: the pre-existing ids are preserved");
      check(n["LogLevel"].as<int>(-1) == 2, "repair: the rest of the doc is intact");
    }
  }

  // 2) A well-formed uniform ZERO-indent list must be left BYTE-IDENTICAL
  //    (never rewrite a good file -> the FileWatcher does not thrash).
  {
    const std::string good =
        "AdditionalApps:\n"
        "- 111\n"
        "- 222\n"
        "LogLevel: 2\n";
    bool changed = true;
    const std::string out = ConfNormalize::repairSeqIndent(good, &changed);
    check(!changed, "idempotent: uniform 0-indent list reports no change");
    check(out == good, "idempotent: uniform 0-indent list is byte-identical");
  }

  // 3) A well-formed uniform TWO-indent list must be left BYTE-IDENTICAL.
  {
    const std::string good =
        "AdditionalApps:\n"
        "  - 111\n"
        "  - 222\n"
        "LogLevel: 2\n";
    bool changed = true;
    const std::string out = ConfNormalize::repairSeqIndent(good, &changed);
    check(!changed, "idempotent: uniform 2-indent list reports no change");
    check(out == good, "idempotent: uniform 2-indent list is byte-identical");
  }

  // 4) A sequence-of-maps (continuation lines that are NOT '-' items) must not
  //    be disturbed: the '-' lines are already consistent, the 'key: val'
  //    continuation lines must be left untouched.
  {
    const std::string nested =
        "DlcData:\n"
        "  1234:\n"
        "    - name: a\n"
        "      id: 1\n"
        "    - name: b\n"
        "      id: 2\n"
        "LogLevel: 2\n";
    bool changed = true;
    const std::string out = ConfNormalize::repairSeqIndent(nested, &changed);
    check(!changed, "nested seq-of-maps: reports no change");
    check(out == nested, "nested seq-of-maps: byte-identical");
  }

  // 5) DenuvoGames-style nested (mapping key -> flat list) stays intact.
  {
    const std::string denuvo =
        "DenuvoGames:\n"
        "  76561198000000000:\n"
        "    - 12345\n"
        "    - 67890\n"
        "LogLevel: 2\n";
    bool changed = true;
    const std::string out = ConfNormalize::repairSeqIndent(denuvo, &changed);
    check(!changed, "denuvo nested list: reports no change");
    check(out == denuvo, "denuvo nested list: byte-identical");
  }

  // 6) Corruption in the MIDDLE of an otherwise-uniform list (one odd item).
  //    yaml-cpp does not throw here -- it SILENTLY folds "111" and "222" into a
  //    single scalar "111 - 222" (2 elements, one of them junk), losing an id.
  //    The repair splits them back into three clean scalar ids.
  {
    const std::string bad =
        "AdditionalApps:\n"
        "- 111\n"
        "  - 222\n"
        "- 333\n"
        "LogLevel: 2\n";
    // Precondition: it "parses" but is silently wrong (not 3 clean ids).
    {
      auto apps = YAML::Load(bad)["AdditionalApps"];
      check(!(apps.IsSequence() && apps.size() == 3),
            "precondition: mid-list odd indent is silently mis-parsed");
    }
    bool changed = false;
    const std::string fixed = ConfNormalize::repairSeqIndent(bad, &changed);
    check(changed && parses(fixed), "repair: mid-list odd indent repaired");
    if (parses(fixed)) {
      auto apps = YAML::Load(fixed)["AdditionalApps"];
      check(apps.IsSequence() && apps.size() == 3,
            "repair: all three mid-list ids survive as clean scalars");
    }
  }

  if (failures) {
    printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  printf("\nall checks passed\n");
  return 0;
}
