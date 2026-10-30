// Build: make test-parental

#include "../src/feats/parental.hpp"

#include <cstdio>
#include <cstdint>
#include <optional>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static void putVarint(std::vector<uint8_t>& out, uint64_t value)
{
	while (value >= 0x80)
	{
		out.push_back(static_cast<uint8_t>(value) | 0x80);
		value >>= 7;
	}
	out.push_back(static_cast<uint8_t>(value));
}

static void putVarintField(std::vector<uint8_t>& out, uint32_t number, uint64_t value)
{
	putVarint(out, static_cast<uint64_t>(number) << 3);
	putVarint(out, value);
}

static void putBytesField(std::vector<uint8_t>& out, uint32_t number,
	                      const std::vector<uint8_t>& value)
{
	putVarint(out, (static_cast<uint64_t>(number) << 3) | 2);
	putVarint(out, value.size());
	out.insert(out.end(), value.begin(), value.end());
}

static std::optional<uint64_t> varintField(const std::vector<uint8_t>& body,
	                                       uint32_t number)
{
	std::optional<uint64_t> found;
	Parental::Wire::walk(body.data(), body.size(), [&](const Parental::Wire::Field& field) {
		if (field.number != number || field.wireType != 0) return;
		std::size_t pos = field.valueOff;
		uint64_t value = 0;
		if (Parental::Wire::readVarint(body.data(), body.size(), pos, value))
		{
			found = value;
		}
	});
	return found;
}

static bool hasField(const std::vector<uint8_t>& body, uint32_t number)
{
	bool found = false;
	Parental::Wire::walk(body.data(), body.size(), [&](const Parental::Wire::Field& field) {
		if (field.number == number) found = true;
	});
	return found;
}

int main()
{
	{
		// x86-64 beta 1793235415 0x1908b40.
		const uint8_t receiverPrologue[] = {
			0x41, 0x57, 0x41, 0x56, 0x45, 0x89, 0xc6,
			0x41, 0x55, 0x49, 0x89, 0xf5,
			0x41, 0x54, 0x41, 0x89, 0xd4,
			0x55, 0x48, 0x89, 0xfd, 0x53, 0x48, 0x89, 0xcb,
			0x48, 0x81, 0xec, 0x58, 0x02, 0x00, 0x00,
		};
		CHECK(Parental::looksLikeSettingsReceiver(receiverPrologue,
		      sizeof(receiverPrologue)),
		      "Linux parental settings receiver prologue is accepted");
		auto badReceiver = std::vector<uint8_t>(
			receiverPrologue, receiverPrologue + sizeof(receiverPrologue));
		badReceiver[5] = 0x90;
		CHECK(!Parental::looksLikeSettingsReceiver(
		          badReceiver.data(), badReceiver.size()),
		      "unexpected parental settings receiver prologue is rejected");
	}

	{
		std::vector<uint8_t> settings;
		putVarintField(settings, 1, 76561198000000000ULL);
		putVarintField(settings, 9, 1);
		putVarintField(settings, 10, 0x1800);
		putVarintField(settings, 13, 0);
		putBytesField(settings, 15, { 0x08, 0x01 });
		putBytesField(settings, 16, { 0x08, 0x01 });
		putVarintField(settings, 20, 7);

		auto rewritten = Parental::rewriteSettings(settings.data(), settings.size());
		CHECK(rewritten.has_value(), "valid parental settings are rewritten");
		if (rewritten)
		{
			CHECK(varintField(*rewritten, 9) == 1, "is_enabled remains active");
			CHECK(varintField(*rewritten, 10) == 0x7fffULL,
			      "enabled_features allows every locally unlockable feature");
			CHECK(varintField(*rewritten, 13) == 0x7fffULL,
			      "temporary_enabled_features allows every locally unlockable feature");
			CHECK(!hasField(*rewritten, 15), "playtime restrictions are removed");
			CHECK(!hasField(*rewritten, 16), "temporary playtime restrictions are removed");
			CHECK(varintField(*rewritten, 20) == 7, "unrelated settings are preserved");
		}
	}

	{
		std::vector<uint8_t> settings;
		putVarintField(settings, 1, 76561198000000000ULL);
		putVarintField(settings, 9, 0);
		putVarintField(settings, 10, 0x1234);

		auto rewritten = Parental::rewriteSettings(settings.data(), settings.size());
		CHECK(rewritten == settings,
		      "already-disabled parental settings are left unchanged");
	}

	{
		const uint8_t malformed[] = { 0x0a, 0x7f };
		CHECK(!Parental::rewriteSettings(malformed, sizeof(malformed)).has_value(),
		      "malformed parental settings are rejected");
		const uint8_t overflowingVarint[] = {
			0x48, 0x80, 0x80, 0x80, 0x80, 0x80,
			0x80, 0x80, 0x80, 0x80, 0x02,
		};
		CHECK(!Parental::rewriteSettings(overflowingVarint,
		      sizeof(overflowingVarint)).has_value(),
		      "overflowing protobuf varints are rejected");
		const uint8_t oversizedField[] = { 0x80, 0x80, 0x80, 0x80, 0x10, 0x00 };
		CHECK(!Parental::rewriteSettings(oversizedField,
		      sizeof(oversizedField)).has_value(),
		      "protobuf field numbers above the valid range are rejected");
	}

	if (g_failures == 0)
	{
		std::printf("\ntest_parental: ALL PASS\n");
		return 0;
	}
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
