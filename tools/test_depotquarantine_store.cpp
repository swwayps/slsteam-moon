// Standalone test for the persistent DLC quarantine record store.
//
// Build:
//   g++ -std=c++20 tools/test_depotquarantine_store.cpp
//       -o /tmp/test_depotquarantine_store && /tmp/test_depotquarantine_store

#include "../src/feats/depotquarantine_store.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

namespace QS = DepotQuarantineStore;

int main()
{
	const std::string badKey(32, '\x11');
	const std::string otherKey(32, '\x22');

	// --- key fingerprints -------------------------------------------------
	const uint64_t fp = QS::fingerprintKey(badKey);
	CHECK(fp != 0, "a 32-byte key produces a non-zero fingerprint");
	CHECK(fp == QS::fingerprintKey(badKey), "fingerprint is deterministic");
	CHECK(fp != QS::fingerprintKey(otherKey),
	      "a different key produces a different fingerprint");
	CHECK(QS::fingerprintKey("short") == 0,
	      "an invalid key length yields no fingerprint");
	CHECK(QS::fingerprintKey("") == 0, "an empty key yields no fingerprint");

	// --- upsert semantics -------------------------------------------------
	std::vector<QS::Record> records;
	CHECK(QS::upsert(records, {1902690, 2473120, 2473120, fp}),
	      "a new record is inserted");
	CHECK(records.size() == 1, "inserting once keeps a single record");
	CHECK(!QS::upsert(records, {1902690, 2473120, 2473120, fp}),
	      "re-inserting an identical record changes nothing");
	CHECK(records.size() == 1, "identical re-insert does not duplicate");

	CHECK(QS::upsert(records, {1902690, 2473121, 2473121, fp}),
	      "a second depot is recorded independently");
	CHECK(records.size() == 2, "distinct depots produce distinct records");

	CHECK(QS::upsert(records, {1902690, 2473120, 2473120,
	                           QS::fingerprintKey(otherKey)}),
	      "a new key fingerprint replaces the record for that depot");
	CHECK(records.size() == 2, "replacement does not grow the record set");

	CHECK(!QS::upsert(records, {0, 2473120, 2473120, fp}),
	      "records without an appId are rejected");
	CHECK(!QS::upsert(records, {1902690, 0, 2473120, fp}),
	      "records without a depotId are rejected");
	CHECK(!QS::upsert(records, {1902690, 2473120, 0, fp}),
	      "base/shared depots are never recorded");
	CHECK(!QS::upsert(records, {1902690, 2473120, 2473120, 0}),
	      "records without a key fingerprint are rejected");

	// --- lookup -----------------------------------------------------------
	std::vector<QS::Record> lookup;
	QS::upsert(lookup, {1902690, 2473120, 2473120, fp});
	CHECK(QS::isQuarantined(lookup, 2473120, fp),
	      "the recorded depot matches its own key fingerprint");
	CHECK(!QS::isQuarantined(lookup, 2473120, QS::fingerprintKey(otherKey)),
	      "a replaced key releases the depot");
	CHECK(!QS::isQuarantined(lookup, 1902696, fp),
	      "an unrelated depot is not quarantined");

	// --- serialization round trip ----------------------------------------
	const std::string text = QS::serialize(records);
	const auto parsed = QS::parse(text);
	CHECK(parsed.size() == records.size(),
	      "serialization round trip preserves the record count");
	bool roundTripped = true;
	for (const auto& record : records)
	{
		if (!QS::isQuarantined(parsed, record.depotId, record.keyFingerprint))
		{
			roundTripped = false;
		}
	}
	CHECK(roundTripped, "every record survives a serialize/parse cycle");

	const auto reparsed = QS::parse(QS::serialize(parsed));
	CHECK(QS::serialize(reparsed) == text, "serialization is stable");

	// --- parser robustness ------------------------------------------------
	CHECK(QS::parse("").empty(), "an empty payload parses to no records");
	CHECK(QS::parse("garbage\n\n# comment\n").empty(),
	      "malformed lines are discarded");
	CHECK(QS::parse("1902690 2473120 2473120\n").empty(),
	      "records with missing fields are discarded");
	CHECK(QS::parse("1902690 2473120 0 12345\n").empty(),
	      "a persisted base depot is discarded on load");

	{
		const auto mixed = QS::parse(
		    "garbage\n1902690 2473120 2473120 12345\nalso bad\n");
		CHECK(mixed.size() == 1, "valid records survive alongside bad lines");
		CHECK(QS::isQuarantined(mixed, 2473120, 12345),
		      "the surviving record keeps its fingerprint");
	}

	// --- package-0 injection filter --------------------------------------
	{
		const std::vector<uint32_t> ids = {1902690, 2473120, 2494230, 2473121};
		const std::unordered_set<uint32_t> drop = {2473120, 2473121};
		const auto kept = QS::withoutIds(ids, drop);
		CHECK(kept.size() == 2, "only the quarantined ids are removed");
		CHECK(kept[0] == 1902690 && kept[1] == 2494230,
		      "the remaining ids keep their original order");
		CHECK(QS::withoutIds(ids, {}).size() == ids.size(),
		      "an empty drop set leaves the list untouched");
		CHECK(QS::withoutIds({}, drop).empty(),
		      "filtering an empty list is a no-op");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
