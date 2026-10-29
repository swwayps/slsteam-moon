// Test for the config parse-with-repair decision layer (confload.hpp).
//
// This is the glue that makes a malformed config.yaml never brick Steam:
//   - a clean file parses as-is (untouched),
//   - a file with inconsistent block-sequence indentation (the LuaTools writer
//     slip) is normalised and parsed, and the caller is told to persist the
//     fix (self-heal) so the user's game list survives,
//   - a file that cannot be salvaged reports Failed so the caller falls back to
//     defaults and Steam still boots.
// It NEVER throws, so it cannot rely on the release build's (unreliable)
// exception landing pads.
//
// Build:
//   g++ -O3 -flto=auto -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
//       -I include -I src tools/test_confload.cpp lib/libyaml-cpp.a \
//       -lpthread -o /tmp/test_confload && /tmp/test_confload

#include "confload.hpp"
#include "yaml-cpp/yaml.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool cond, const char* msg) {
  if (cond) printf("OK:   %s\n", msg);
  else { printf("FAIL: %s\n", msg); failures++; }
}

int main() {
  using ConfLoad::Outcome;

  // 1) The exact field corruption: mixed 2/0-space AdditionalApps items.
  {
    const std::string bad =
        "API: false\n"
        "AdditionalApps:\n"
        "  - 4149320   # added via LuaTools\n"
        "- 381210\n"
        "- 489980\n"
        "LogLevel: 2\n";
    YAML::Node node;
    std::string repaired;
    auto outcome = ConfLoad::parseWithRepair(bad, node, repaired);
    check(outcome == Outcome::Repaired, "malformed config -> Repaired");
    check(node["AdditionalApps"].IsSequence() && node["AdditionalApps"].size() == 3,
          "malformed config -> all 3 ids parsed");
    check(node["LogLevel"].as<int>(-1) == 2, "malformed config -> rest intact");
    // repaired text is what the caller persists; it must itself parse.
    bool reparse = true;
    try { YAML::Load(repaired); } catch (...) { reparse = false; }
    check(reparse, "malformed config -> persisted repair re-parses");
  }

  // 2) A clean file parses as-is and is NOT flagged for rewrite.
  {
    const std::string good =
        "AdditionalApps:\n"
        "  - 111\n"
        "  - 222\n"
        "LogLevel: 2\n";
    YAML::Node node;
    std::string repaired;
    auto outcome = ConfLoad::parseWithRepair(good, node, repaired);
    check(outcome == Outcome::ParsedAsIs, "clean config -> ParsedAsIs");
    check(node["AdditionalApps"].size() == 2, "clean config -> parsed");
  }

  // 3) Unsalvageable garbage -> Failed, node empty (caller uses defaults).
  {
    const std::string garbage = "foo: [1, 2\nbar: {\n"; // unterminated flow
    YAML::Node node;
    std::string repaired;
    auto outcome = ConfLoad::parseWithRepair(garbage, node, repaired);
    check(outcome == Outcome::Failed, "garbage config -> Failed");
    check(!node["foo"], "garbage config -> node empty/defaulted");
  }

  // 4) Silent-fold corruption: yaml-cpp does not throw but folds two ids into
  //    one scalar. parseWithRepair must PREFER the repaired (clean) parse.
  {
    const std::string bad =
        "AdditionalApps:\n"
        "- 111\n"
        "  - 222\n"
        "- 333\n"
        "LogLevel: 2\n";
    YAML::Node node;
    std::string repaired;
    auto outcome = ConfLoad::parseWithRepair(bad, node, repaired);
    check(outcome == Outcome::Repaired, "silent-fold config -> Repaired");
    check(node["AdditionalApps"].IsSequence() && node["AdditionalApps"].size() == 3,
          "silent-fold config -> 3 clean ids (not the folded 2)");
  }

  // 6) Valid nested lists are not the writer slip: an item that opens a block
  //    keeps its children's deeper indent and the file is not rewritten.
  {
    YAML::Node node;
    std::string repaired;
    auto outcome = ConfLoad::parseWithRepair(
        "X:\n  - ids:\n      - 1\n      - 2\n  - other\n", node, repaired);
    check(outcome == Outcome::ParsedAsIs &&
          node["X"].size() == 2 && node["X"][0]["ids"].size() == 2 &&
          node["X"][1].as<std::string>("") == "other",
          "a mapping item with a nested list is kept as written");
    outcome = ConfLoad::parseWithRepair(
        "X:\n  - - a\n    - b\n  -\n    - c\n  - d\n", node, repaired);
    check(outcome == Outcome::ParsedAsIs &&
          node["X"].size() == 3 && node["X"][0].size() == 2 &&
          node["X"][1].size() == 1 && node["X"][2].as<std::string>("") == "d",
          "inline and bare nested sequences are kept as written");
    outcome = ConfLoad::parseWithRepair(
        "X:\n  - note: |-  # text\n      - not an item\n  - y\n", node, repaired);
    check(outcome == Outcome::ParsedAsIs &&
          node["X"][0]["note"].as<std::string>("") == "- not an item",
          "a block scalar whose text starts with a dash is kept as written");
  }
  // 5) Empty file parses as-is (a null document; defaults kick in downstream).
  {
    YAML::Node node;
    std::string repaired;
    auto outcome = ConfLoad::parseWithRepair("", node, repaired);
    check(outcome == Outcome::ParsedAsIs, "empty config -> ParsedAsIs");
  }

  // 6) A document that is not a map (plain text, a lone list) is a parse
  //    failure: reading a key from a scalar or list root throws.
  {
    YAML::Node node;
    std::string repaired;
    check(ConfLoad::parseWithRepair("just text\n", node, repaired) == Outcome::Failed
          && node.IsNull(), "scalar config -> Failed with an empty node");
    check(ConfLoad::parseWithRepair("- 1\n- 2\n", node, repaired) == Outcome::Failed,
          "list config -> Failed");
  }
  if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
  printf("\nall checks passed\n");
  return 0;
}
