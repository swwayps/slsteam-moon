// The manifest-pin builder locator against a 64-bit steamclient. Its i386
// caller-site signature (and the push-literal flag pairs this test checked) has
// no x86-64 counterpart yet, so it must compile with an empty signature: the
// pin then stays off instead of hooking the wrong call.
// Run: make test-manifestpin-patterns [STEAMCLIENT=...]
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::cerr << "usage: test_manifestpin_patterns <steamclient.so> <src/patterns.cpp>\n";
		return 2;
	}
	std::ifstream sourceInput(argv[2], std::ios::binary);
	const std::string source{std::istreambuf_iterator<char>(sourceInput),
	                         std::istreambuf_iterator<char>()};
	const std::string literal = "\"CDepotDownloadMgr::BuildDepotDependency\",";
	std::size_t at = source.find(literal);
	if (at != std::string::npos)
		at = source.find_first_not_of(" \t\r\n", at + literal.size());
	if (at == std::string::npos || source.compare(at, 3, "\"\",") != 0)
	{
		std::cerr << "CDepotDownloadMgr::BuildDepotDependency is not ported to x86-64 yet, "
		             "so its signature must stay empty\n";
		return 1;
	}
	std::cout << "manifest-pin builder locator stays off on x86-64\n";
	return 0;
}
