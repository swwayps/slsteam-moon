// Source wiring test: no exception may leave an rtld-audit callback, so the
// decompiler pass that la_objopen runs is contained, and so is the logging in
// the handlers of setup() and load(), which often run after a bad_alloc.
// Build (from repo root):
//   g++ -std=c++20 -Wall -Wextra -Wpedantic tools/test_audit_guards.cpp -o /tmp/test_audit_guards && /tmp/test_audit_guards
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
// `call` sits between the closest `try` before it and that try's first
// handler, which is a catch (...).
bool contained(const std::string& body, const std::string& call)
{
	const auto at = body.find(call);
	if (at == npos) return false;
	const auto open = body.rfind("try", at);
	const auto close = body.find("catch (...)", at);
	return open != npos && close != npos && body.find("catch", open) == close;
}
}
int main()
{
	std::ifstream stream("src/main.cpp");
	const std::string source{std::istreambuf_iterator<char>(stream), {}};
	const std::string decompile =
		between(source, "static void decompileBeforeRelocation(", "\n}\n");
	const std::string guarded = between(source, "static void setupGuarded()", "\n}\n");
	const std::string load = between(source, "void load()", "\n}\n");
	expect(!decompile.empty() && !guarded.empty() && !load.empty(),
	       "the audit entry points are found");
	expect(contained(decompile, "Decompiler::parseModule(") &&
	           contained(decompile, "vft.analyze()"),
	       "the decompiler pass run from la_objopen is contained");
	expect(contained(guarded, "setup() aborted by exception") &&
	           contained(guarded, "setup() aborted by a non-std exception"),
	       "the setup() handlers log inside their own try");
	expect(contained(load, "load() aborted by exception") &&
	           contained(load, "load() aborted by a non-std exception"),
	       "the load() handlers log inside their own try");
	return failures == 0 ? 0 : 1;
}
