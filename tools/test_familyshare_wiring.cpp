// Source-contract coverage for the CM receive hook, which carries the
// Family Share choke and the achievements reply rewrite.
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
	const std::string cmrecv = withoutWhitespace(readFile("src/feats/cmrecv.cpp"));
	const std::string manifestcode = withoutWhitespace(readFile("src/feats/manifestcode.cpp"));
	// Achievement owner replies are CM responses, so they are rewritten on this
	// hook; no other receive path may carry a second copy of that rewrite. A
	// service-method answer to the donor's request-code job is read first and
	// is not an achievements reply.
	expect(
		cmrecv.find("casek_EMsgServiceMethodResponse:"
		            "if(ManifestCode::recvServiceMethodResponse(pkt))break;[[fallthrough]];"
		            "casek_EMsgClientGetUserStatsResponse:"
		            "Achievements::recvReply(pkt);") != std::string::npos,
		"achievement replies are rewritten on the CM receive hook");
	expect(
		manifestcode.find("rewriteResponse") == std::string::npos,
		"no second achievements reply path remains");

	// CCMInterface::RecvPkt is the only CM receive transport: it carries the
	// Family Share choke and every CM message feature, so it must be part of
	// the mandatory hook chain instead of an optional extra.
	expect(
		hooks.find(
			"boolsucceeded=CCMInterface_RecvPkt.setup(VFTIndexes::CCMInterface::RecvPkt,&hkCMInterface_RecvPkt)")
			!= std::string::npos,
		"CM receive setup is part of the mandatory hook chain");
	expect(
		hooks.find("familyShareHookReady") == std::string::npos,
		"no optional readiness flag remains");
	expect(
		hooks.find("CCMInterface_RecvPkt.place();") != std::string::npos,
		"CM receive hook is placed");
	expect(
		hooks.find("CCMInterface_RecvPkt.remove();") != std::string::npos,
		"CM receive hook is removed on teardown");
	expect(
		hooks.find("CProtoBufMsgBase_InitFromPacket") == std::string::npos,
		"the classic InitFromPacket transport is gone");
	expect(
		patterns.find("CCMInterface::RecvPkt") == std::string::npos,
		"CM receive is located by its vtable slot, not a pattern");
	if (failures != 0)
	{
		std::cerr << failures << " CM receive wiring check(s) failed\n";
		return 1;
	}
	std::cout << "CM receive hook wiring is mandatory\n";
	return 0;
}
