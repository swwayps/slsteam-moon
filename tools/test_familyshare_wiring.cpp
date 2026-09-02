// Source-contract coverage for the optional Family Share receive hook.
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
int failures = 0;

std::string readFile(const char* path)
{
	std::ifstream input(path);
	if (!input)
	{
		std::cerr << "could not open " << path << '\n';
		std::exit(2);
	}
	return {std::istreambuf_iterator<char>(input),
	        std::istreambuf_iterator<char>()};
}

std::string withoutWhitespace(std::string_view input)
{
	std::string output;
	output.reserve(input.size());
	for (const unsigned char character : input)
	{
		if (!std::isspace(character))
			output.push_back(character);
	}
	return output;
}

void expect(bool condition, std::string_view message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}
} // namespace

int main()
{
	const std::string hooks = withoutWhitespace(readFile("src/hooks.cpp"));
	const std::string patterns = withoutWhitespace(readFile("src/patterns.cpp"));

	expect(
		hooks.find("&&CCMInterface_RecvPkt.setup(") == std::string::npos,
		"Family Share setup is outside the mandatory hook chain");
	expect(
		hooks.find(
			"familyShareHookReady=CCMInterface_RecvPkt.setup(")
			!= std::string::npos,
		"optional setup result is retained");
	expect(
		hooks.find(
			"if(familyShareHookReady){CCMInterface_RecvPkt.place();}")
			!= std::string::npos,
		"optional hook placement is guarded");
	expect(
		hooks.find(
			"if(familyShareHookReady){CCMInterface_RecvPkt.remove();"
			"familyShareHookReady=false;}")
			!= std::string::npos,
		"optional hook removal is guarded and resets state");
	expect(
		patterns.find("CCMInterface::RecvPkt.optional=true;")
			!= std::string::npos,
		"Family Share locator is marked optional before catalog loading");

	if (failures != 0)
	{
		std::cerr << failures << " Family Share wiring check(s) failed\n";
		return 1;
	}
	std::cout << "Family Share hook wiring is optional and guarded\n";
	return 0;
}
