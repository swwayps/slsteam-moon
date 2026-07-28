// Standalone tests for the client-facing app ownership ticket provider.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_appticket.cpp -o /tmp/test_appticket && /tmp/test_appticket


#include "../src/feats/appticket.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static void put32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value)
{
	std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

static std::vector<uint8_t> signedTicket(uint32_t appId, uint8_t signatureSeed)
{
	// Hand-built ticket shape:
	//   [20 bytes signed payload][128 byte signature]
	// The first uint32 names the signature offset.  SteamID and AppID live at
	// offsets 8 and 16 in the signed payload.
	std::vector<uint8_t> ticket(20 + 128, 0);
	put32(ticket, 0, 20);
	put32(ticket, 16, appId);
	for (std::size_t i = 0; i < 128; ++i)
	{
		ticket[20 + i] = static_cast<uint8_t>(signatureSeed + i);
	}
	return ticket;
}

int main()
{
	// Mutation caught: forgetting the inserted AppID or reporting physical
	// size instead of the intentionally four-byte-short logical size.
	{
		const auto source = signedTicket(7, 0x40);
		const auto derived = AppTicket::deriveLocalOwnershipTicket(source, 1817230);

		CHECK(derived.source == AppTicket::Source::LocalDerived,
		      "derive: reports local-derived source");
		CHECK(derived.data.size() == source.size() + sizeof(uint32_t),
		      "derive: inserts exactly four physical bytes");
		CHECK(derived.totalSize == source.size(),
		      "derive: preserves the source logical size");
		CHECK(derived.appIdOffset == 20 && derived.signatureOffset == 24,
		      "derive: points AppID at insertion and signature after it");
		CHECK(derived.steamIdOffset == 8 && derived.signatureSize == 128,
		      "derive: preserves SteamID and signature metadata");

		uint32_t embeddedAppId = 0;
		std::memcpy(&embeddedAppId, derived.data.data() + derived.appIdOffset,
		            sizeof(embeddedAppId));
		CHECK(embeddedAppId == 1817230,
		      "derive: embeds the requested AppID in little-endian form");
		CHECK(std::equal(source.begin(), source.begin() + 20, derived.data.begin()),
		      "derive: preserves the signed payload byte-for-byte");
		CHECK(std::equal(source.begin() + 20, source.end(),
		                 derived.data.begin() + derived.signatureOffset),
		      "derive: moves the original signature byte-for-byte");
	}

	// Mutation caught: accepting truncated or internally inconsistent source
	// tickets and then reading beyond their bounds.
	{
		auto shortTicket = std::vector<uint8_t>(127, 0);
		auto badOffset = signedTicket(7, 0x10);
		put32(badOffset, 0, 21);

		CHECK(!AppTicket::deriveLocalOwnershipTicket(shortTicket, 1817230),
		      "derive: rejects a ticket shorter than its signature");
		CHECK(!AppTicket::deriveLocalOwnershipTicket(badOffset, 1817230),
		      "derive: rejects a signature offset inconsistent with physical size");
		CHECK(!AppTicket::deriveLocalOwnershipTicket(signedTicket(7, 0x10), 0),
		      "derive: rejects a zero target AppID");
		CHECK(!AppTicket::deriveLocalOwnershipTicket(signedTicket(2050650, 0x10), 1817230),
		      "derive: rejects a source ticket that does not belong to AppID 7");
	}

	// Mutation caught: silently replacing an explicit signed ticket with the
	// local-derived fallback.
	{
		const auto explicitBytes = signedTicket(1817230, 0x20);
		const auto localBytes = signedTicket(7, 0x60);
		const auto selected = AppTicket::prepareOwnershipTicket(
			explicitBytes, localBytes, 1817230);

		CHECK(selected.source == AppTicket::Source::Explicit,
		      "select: valid explicit ticket has priority");
		CHECK(selected.data == explicitBytes,
		      "select: explicit ticket is returned unchanged");
		CHECK(selected.totalSize == explicitBytes.size()
		      && selected.appIdOffset == 16 && selected.signatureOffset == 20,
		      "select: explicit ticket metadata matches its signed layout");
	}

	// Mutation caught: accepting a correctly shaped explicit ticket belonging
	// to another app instead of using the safe local fallback.
	{
		const auto wrongExplicit = signedTicket(2050650, 0x20);
		const auto localBytes = signedTicket(7, 0x60);
		const auto selected = AppTicket::prepareOwnershipTicket(
			wrongExplicit, localBytes, 1817230);

		CHECK(selected.source == AppTicket::Source::LocalDerived,
		      "select: explicit ticket for another AppID falls back to local derivation");
		uint32_t embeddedAppId = 0;
		std::memcpy(&embeddedAppId, selected.data.data() + selected.appIdOffset,
		            sizeof(embeddedAppId));
		CHECK(embeddedAppId == 1817230,
		      "select: fallback embeds the requested AppID");
	}

	// Mutation caught: copying a partial ticket or publishing output metadata
	// when the caller's buffer is too small.
	{
		const auto prepared = AppTicket::deriveLocalOwnershipTicket(
			signedTicket(7, 0x30), 1817230);
		std::array<uint8_t, 256> buffer{};
		uint32_t appOffset = 99;
		uint32_t steamOffset = 99;
		uint32_t signatureOffset = 99;
		uint32_t signatureSize = 99;

		CHECK(!AppTicket::copyOwnershipTicket(
			prepared, buffer.data(), prepared.data.size() - 1,
			&appOffset, &steamOffset, &signatureOffset, &signatureSize),
		      "copy: rejects a destination one byte too small");
		CHECK(appOffset == 99 && steamOffset == 99
		      && signatureOffset == 99 && signatureSize == 99,
		      "copy: leaves metadata untouched on failure");

		CHECK(AppTicket::copyOwnershipTicket(
			prepared, buffer.data(), buffer.size(),
			&appOffset, &steamOffset, &signatureOffset, &signatureSize),
		      "copy: writes a valid ticket into a sufficient buffer");
		CHECK(std::equal(prepared.data.begin(), prepared.data.end(), buffer.begin()),
		      "copy: writes every physical ticket byte");
		CHECK(appOffset == prepared.appIdOffset
		      && steamOffset == prepared.steamIdOffset
		      && signatureOffset == prepared.signatureOffset
		      && signatureSize == prepared.signatureSize,
		      "copy: publishes every output offset");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
