// The ReconcilePin locators against a 64-bit steamclient. The builder
// (Patterns::CUserAppManager::BuildDepotDependency, upstream dev-X86_64) must
// match once; EvaluateConfigChanges and its call-site walk are i386-only, so
// that locator must compile with an empty signature and ReconcilePin stays off.
// Build with: g++ -std=c++20 -I src tools/test_reconcilepin_pattern.cpp -o /tmp/test_reconcilepin_pattern
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
namespace
{
std::string readFile(const char* path)
{
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: test_reconcilepin_pattern /path/to/steamclient.so\n";
		return 2;
	}
	const std::string image = readFile(argv[1]);
	const std::string source = readFile("src/patterns.cpp");
	if (image.empty() || source.empty())
	{
		std::cerr << "could not read " << argv[1] << " or src/patterns.cpp\n";
		return 2;
	}
	const std::string builder = "\x4D\x85\xFF\x88\x44\x24";
	if (source.find("\"4D 85 FF 88 44 24\"") == std::string::npos)
	{
		std::cerr << "the builder signature is not the one compiled in src/patterns.cpp\n";
		return 1;
	}
	const std::size_t first = image.find(builder);
	if (first == std::string::npos || image.find(builder, first + 1) != std::string::npos)
	{
		std::cerr << "expected one CUserAppManager::BuildDepotDependency match\n";
		return 1;
	}
	const std::string literal = "\"CDepotDownloadMgr::EvaluateConfigChanges\",";
	std::size_t at = source.find(literal);
	if (at != std::string::npos)
		at = source.find_first_not_of(" \t\r\n", at + literal.size());
	if (at == std::string::npos || source.compare(at, 3, "\"\",") != 0)
	{
		std::cerr << "EvaluateConfigChanges is not ported to x86-64 yet, "
		             "so its signature must stay empty\n";
		return 1;
	}
	std::cout << "builder matches once; ReconcilePin stays off on x86-64\n";
	return 0;
}
