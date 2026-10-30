// Source wiring test: the ReconcilePin hooks run on Steam's content thread, so
// a throw from their patch work must not unwind into Steam, and Steam's own
// function still runs exactly once, outside the try.
// Build (from repo root):
//   g++ -std=c++20 -Wall -Wextra -Wpedantic tools/test_reconcilepin_wiring.cpp -o /tmp/test_reconcilepin_wiring && /tmp/test_reconcilepin_wiring
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
namespace
{
constexpr auto npos = std::string::npos;
int failures = 0;
void expect(bool condition, const char* message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message);
	if (!condition) ++failures;
}
std::string between(const std::string& text, const std::string& from, const std::string& to)
{
	const auto begin = text.find(from);
	if (begin == npos) return {};
	const auto end = text.find(to, begin);
	return text.substr(begin, end == npos ? npos : end - begin);
}
// `call` sits between a `try` and its `catch (...)`.
bool contained(const std::string& body, const std::string& call)
{
	const auto at = body.find(call);
	const auto open = body.rfind("try", at);
	const auto close = body.find("catch (...)", at);
	return at != npos && open != npos && close != npos &&
	       body.find("catch", open) == close;
}
}
int main()
{
	std::ifstream stream("src/feats/reconcilepin.cpp");
	const std::string source{std::istreambuf_iterator<char>(stream), {}};
	const std::string build = between(source, "void* hkBuildTarget(", "\n\t}\n");
	const std::string evaluate = between(source, "void* hkEvaluate(", "\n\t}\n");
	expect(!build.empty() && !evaluate.empty(), "both ReconcilePin hooks are found");
	expect(contained(build, "patchTargetVec(") && !contained(build, "g_origBuild("),
	       "the target-vector patch is contained and the builder runs outside it");
	expect(contained(evaluate, "patchTargetCtx(") &&
	           !contained(evaluate, "g_orig(") &&
	           evaluate.find("return g_orig(") > evaluate.rfind("catch (...)"),
	       "the context patch is contained and EvaluateConfigChanges runs after it");
	return failures == 0 ? 0 : 1;
}
