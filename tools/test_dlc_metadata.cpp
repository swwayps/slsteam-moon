#include "feats/dlc_metadata.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_set>

namespace
{
int failures = 0;
void check(bool condition, const char* message)
{
	if (condition) return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}
}

int main()
{
	check(DlcMetadata::cacheGenerationMatches(
		/*record=*/7, /*expected=*/7, /*current=*/7),
		"runtime metadata cache accepts the exact managed generation");
	check(!DlcMetadata::cacheGenerationMatches(
		/*record=*/6, /*expected=*/7, /*current=*/7),
		"runtime metadata cache rejects a prior managed generation");
	check(DlcMetadata::cacheGenerationMatches(
		/*record=*/7, /*expected=*/0, /*current=*/0),
		"cold boot accepts a cross-process generation after identity validation");
	check(!DlcMetadata::cacheGenerationMatches(
		/*record=*/7, /*expected=*/0, /*current=*/8),
		"remove and re-add rejects a stale sidecar even without an explicit token");
	check(DlcMetadata::cacheGenerationMatches(
		/*record=*/8, /*expected=*/0, /*current=*/8),
		"runtime cache discovery accepts the current remove/re-add generation");
	check(DlcMetadata::baseIdentityMatches(
		10, std::string(20, 'a'), 10, std::string(20, 'a')),
		"child metadata accepts the exact validated base identity");
	check(!DlcMetadata::baseIdentityMatches(
		10, std::string(20, 'a'), 11, std::string(20, 'a')) &&
		!DlcMetadata::baseIdentityMatches(
			10, std::string(20, 'a'), 10, std::string(20, 'b')) &&
		!DlcMetadata::baseIdentityMatches(
			10, std::string(19, 'a'), 10, std::string(19, 'a')),
		"child metadata rejects changed or malformed base identity");
	check(DlcMetadata::publicationMatchesBase(
		/*recordGeneration=*/7, /*expectedGeneration=*/7,
		/*currentGeneration=*/7, /*recordChange=*/10,
		std::string(20, 'a'), /*currentChange=*/10,
		std::string(20, 'a')),
		"metadata publication accepts the unchanged base pair");
	check(!DlcMetadata::publicationMatchesBase(
		/*recordGeneration=*/7, /*expectedGeneration=*/7,
		/*currentGeneration=*/7, /*recordChange=*/10,
		std::string(20, 'a'), /*currentChange=*/11,
		std::string(20, 'b')) &&
		!DlcMetadata::publicationMatchesBase(
			/*recordGeneration=*/7, /*expectedGeneration=*/7,
			/*currentGeneration=*/8, /*recordChange=*/10,
			std::string(20, 'a'), /*currentChange=*/10,
			std::string(20, 'a')),
		"metadata publication rejects base replacement and remove-readd races");

	const std::string raw =
		"\"appinfo\"\n{\n"
		"\t\"appid\"\t\"2778580\"\n"
		"\t\"common\"\n\t{\n"
		"\t\t\"name\"\t\"Shadow of the Erdtree\"\n"
		"\t\t\"type\"\t\"DLC\"\n"
		"\t\t\"parent\"\t\"1245620\"\n"
		"\t}\n"
		"\t\"extended\"\n\t{\n"
		"\t\t\"dlcforappid\"\t\"1245620\"\n"
		"\t}\n"
		"\t\"depots\"\n\t{\n"
		"\t\t\"2778580\"\n\t\t{\n"
		"\t\t\t\"manifests\"\n\t\t\t{\n"
		"\t\t\t\t\"public\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"gid\"\t\"999999\"\n"
		"\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n"
		"}\n";

	std::string normalized;
	check(DlcMetadata::normalize(raw, 2778580, 1245620, normalized),
		"matching DLC metadata normalizes");
	check(normalized.find("Shadow of the Erdtree") != std::string::npos,
		"UI-facing common metadata is retained");
	check(normalized.find("dlcforappid") != std::string::npos,
		"parent metadata is retained");
	check(normalized.find("depots") == std::string::npos &&
		normalized.find("gid") == std::string::npos &&
		normalized.find("manifests") == std::string::npos,
		"content and manifest topology is stripped from metadata-only output");

	std::string ignored;
	check(!DlcMetadata::normalize(
		"\"appinfo\" { \"public_only\" \"1\" }",
		3655690, 1245620, ignored),
		"public_only technical row is rejected");
	check(!DlcMetadata::normalize(raw, 2778590, 1245620, ignored),
		"mismatched child identity is rejected");
	check(!DlcMetadata::normalize(raw, 2778580, 999, ignored),
		"mismatched parent is rejected");
	std::string nulTerminated = raw;
	nulTerminated.push_back('\0');
	check(DlcMetadata::normalize(
		nulTerminated, 2778580, 1245620, ignored),
		"CM wire with its protobuf NUL terminator is accepted");

	const std::string conflictingParent =
		"\"appinfo\" { \"appid\" \"2778580\" \"common\" { "
		"\"type\" \"dlc\" \"parent\" \"1245620\" } \"extended\" { "
		"\"dlcforappid\" \"999\" } }";
	check(!DlcMetadata::normalize(
		conflictingParent, 2778580, 1245620, ignored),
		"conflicting parent fields fail closed");

	const DlcMetadata::CacheRecord record{
		.baseAppId = 1245620,
		.baseGeneration = 7,
		.baseChangeNumber = 37835451,
		.baseSha = std::string(20, 'b'),
		.apps = {{2778580, 55, std::string(20, 's'), normalized}},
		.rejectedAppIds = {3655690},
	};
	std::unordered_set<std::uint32_t> authoritative{1245620};
	DlcMetadata::appendChildAppIds(record, authoritative);
	check(authoritative ==
		std::unordered_set<std::uint32_t>({1245620, 2778580}),
		"validated metadata children join the local appinfo authority set");
	DlcMetadata::CacheRecord duplicateChildren = record;
	duplicateChildren.apps.push_back({2778580, 0, {}, {}});
	duplicateChildren.apps.push_back({0, 0, {}, {}});
	DlcMetadata::appendChildAppIds(duplicateChildren, authoritative);
	check(authoritative.size() == 2,
		"child authority ignores zero and duplicate appids");
	std::ifstream provisionSource("src/feats/appinfo_provision.cpp");
	const std::string provisionText(
		(std::istreambuf_iterator<char>(provisionSource)),
		std::istreambuf_iterator<char>());
	check(provisionText.find(
		"DlcMetadata::appendChildAppIds(metadata, authoritative)") !=
			std::string::npos,
		"validated sidecars feed child ids into the live authority set");
	std::string encoded;
	check(DlcMetadata::encodeCache(record, encoded),
		"validated metadata cache serializes");
	DlcMetadata::CacheRecord decoded;
	check(DlcMetadata::decodeCache(encoded, decoded) &&
		decoded.baseAppId == 1245620 && decoded.baseGeneration == 7 &&
		decoded.baseChangeNumber == 37835451 && decoded.apps.size() == 1 &&
		decoded.baseSha == std::string(20, 'b') &&
		decoded.apps[0].appid == 2778580 &&
		decoded.apps[0].wireBuffer == normalized &&
		decoded.rejectedAppIds == std::vector<std::uint32_t>({3655690}),
		"validated metadata cache round-trips identity and wire bytes");
	auto malformed = encoded;
	malformed.replace(malformed.find("sha_b64:"), 8, "sha_bad:");
	check(!DlcMetadata::decodeCache(malformed, decoded),
		"metadata cache with missing SHA fails closed");

	return failures == 0 ? 0 : 1;
}
