#include "pattern_catalog.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace
{
	int failures = 0;

	void expect(bool condition, std::string_view message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	const PatternCatalog::ModuleIdentity expected
	{
		"steamclient",
		"steamclient.so",
		std::string(64, 'a'),
		8192,
		std::string(40, 'b'),
	};
	const PatternCatalog::ModuleIdentity steamUiExpected
	{
		"steamui",
		"steamui.so",
		std::string(64, 'a'),
		8192,
		std::string(40, 'b'),
	};

	std::string validToml()
	{
		return
			"schema = 1\n"
			"platform = \"linux32\"\n"
			"component = \"steamclient\"\n"
			"module_name = \"steamclient.so\"\n"
			"module_sha256 = \"" + std::string(64, 'a') + "\"\n"
			"module_size = 8192\n"
			"gnu_build_id = \"" + std::string(40, 'b') + "\"\n"
			"steam_version = 1785347151\n"
			"consumer_commit = \"" + std::string(40, 'c') + "\"\n"
			"minimum_consumer_schema = 1\n"
			"revision = 1\n"
			"source = \"deterministic\"\n"
			"\n"
			"[[locators]]\n"
			"symbol = \"Patterns::Root\"\n"
			"name = \"Root\"\n"
			"target_rva = 4660\n"
			"match_rva = 4656\n"
			"signature = \"55 89 E5\"\n"
			"follow_mode = \"None\"\n"
			"resolver = \"signature\"\n"
			"required = true\n"
			"match_count = 1\n"
			"target_count = 1\n";
	}

	void replaceOnce(std::string& text, std::string_view from, std::string_view to)
	{
		const auto position = text.find(from);
		expect(position != std::string::npos, "test mutation anchor exists");
		if (position != std::string::npos)
			text.replace(position, from.size(), to);
	}

	std::string hotReloadClientToml()
	{
		return validToml() +
			"\n[[locators]]\n"
			"symbol = \"Patterns::CAppInfoCache::GetOrAddAppData\"\n"
			"name = \"CAppInfoCache::GetOrAddAppData\"\n"
			"target_rva = 5000\n"
			"match_rva = 5000\n"
			"signature = \"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5\"\n"
			"follow_mode = \"None\"\n"
			"resolver = \"signature\"\n"
			"required = false\n"
			"match_count = 1\n"
			"target_count = 1\n"
			"\n[[locators]]\n"
			"symbol = \"Patterns::CAppInfoCache::ThreadedReadFromDisk\"\n"
			"name = \"CAppInfoCache::ThreadedReadFromDisk\"\n"
			"target_rva = 5050\n"
			"match_rva = 5050\n"
			"signature = \"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 0C 11 00 00 8B 45 08 89 85 0C EF FF FF\"\n"
			"follow_mode = \"None\"\n"
			"resolver = \"signature\"\n"
			"required = false\n"
			"match_count = 1\n"
			"target_count = 1\n"
			"\n[[locators]]\n"
			"symbol = \"Patterns::CAppInfoCache::SkipFlagReference\"\n"
			"name = \"CAppInfoCache::SkipFlagReference\"\n"
			"target_rva = 5100\n"
			"match_rva = 5100\n"
			"signature = \"80 7E 10 00 0F 44 C8\"\n"
			"follow_mode = \"None\"\n"
			"resolver = \"signature\"\n"
			"required = false\n"
			"match_count = 1\n"
			"target_count = 1\n";
	}

	std::string steamUiToml()
	{
		auto body = validToml();
		replaceOnce(body, "component = \"steamclient\"", "component = \"steamui\"");
		replaceOnce(body, "module_name = \"steamclient.so\"", "module_name = \"steamui.so\"");
		replaceOnce(body, "Patterns::Root", "Patterns::SteamUI::AppControllerRunFrame");
		replaceOnce(body, "name = \"Root\"", "name = \"CSteamUIAppController::RunFrame\"");
		replaceOnce(body, "signature = \"55 89 E5\"",
			"signature = \"55 57 56 53 E8 ? ? ? ?\"");
		replaceOnce(body, "required = true", "required = false");
		return body;
	}

	void expectRejected(std::string body, std::string_view label)
	{
		std::string error;
		auto parsed = PatternCatalog::parseCanonical(body, expected, &error);
		expect(!parsed.has_value(), label);
		expect(!error.empty(), "rejection includes a bounded diagnostic");
	}
}

int main()
{
	std::string error;
	auto parsed = PatternCatalog::parseCanonical(validToml(), expected, &error);
	expect(parsed.has_value(), "literal canonical TOML parses");
	expect(error.empty(), "valid parse has no diagnostic");
	if (parsed)
	{
		expect(parsed->component() == "steamclient", "component is retained");
		expect(parsed->revision() == 1, "revision is retained");
		const auto* root = parsed->entry("Patterns::Root");
		expect(root != nullptr && root->targetRva == 4660, "symbol maps to exact RVA");
		expect(parsed->entry("Patterns::Missing") == nullptr,
		       "unknown symbol has no target");

		const PatternCatalog::CompiledLocator allowed[] =
		{
			{"Patterns::Root", true, "55 89 E5", "None"},
		};
		expect(parsed->validatePolicy(allowed),
		       "metadata matches compiled symbol and required policy");
		const PatternCatalog::CompiledLocator weakened[] =
		{
			{"Patterns::Root", false, "55 89 E5", "None"},
		};
		expect(!parsed->validatePolicy(weakened),
		       "metadata cannot change compiled optionality");
		const PatternCatalog::CompiledLocator unrelated[] =
		{
			{"Patterns::Other", true, "55 89 E5", "None"},
		};
		expect(!parsed->validatePolicy(unrelated),
		       "metadata cannot introduce an uncompiled symbol");
		const PatternCatalog::CompiledLocator signatureMismatch[] =
		{
			{"Patterns::Root", true, "55 8B EC", "None"},
		};
		expect(!parsed->validatePolicy(signatureMismatch),
		       "signature mismatch falls back to the embedded resolver");
		const PatternCatalog::CompiledLocator followMismatch[] =
		{
			{"Patterns::Root", true, "55 89 E5", "Relative"},
		};
		expect(!parsed->validatePolicy(followMismatch),
		       "follow-mode mismatch falls back to the embedded resolver");
		const PatternCatalog::CompiledLocator optionalAbsent[] =
		{
			{"Patterns::Root", true, "55 89 E5", "None"},
			{"Patterns::Optional", false, "8B 45 08", "None"},
		};
		expect(parsed->validatePolicy(optionalAbsent),
		       "an absent optional locator is legal");
		const PatternCatalog::CompiledLocator requiredAbsent[] =
		{
			{"Patterns::Root", true, "55 89 E5", "None"},
			{"Patterns::Required", true, "8B 45 08", "None"},
		};
		expect(!parsed->validatePolicy(requiredAbsent),
		       "an absent required locator rejects the catalog");
	}

	{
		auto client = PatternCatalog::parseCanonical(
			hotReloadClientToml(), expected, &error);
		expect(client.has_value(),
		       "exact client catalog accepts optional function and instruction locators");
		if (client)
		{
			const PatternCatalog::CompiledLocator policy[] =
			{
				{"Patterns::Root", true, "55 89 E5", "None"},
				{"Patterns::CAppInfoCache::GetOrAddAppData", false,
				 "E8 ? ? ? ? 05 ? ? ? ? 55 89 E5", "None"},
				{"Patterns::CAppInfoCache::ThreadedReadFromDisk", false,
				 "55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 0C 11 00 00 8B 45 08 89 85 0C EF FF FF", "None"},
				{"Patterns::CAppInfoCache::SkipFlagReference", false,
				 "80 7E 10 00 0F 44 C8", "None"},
			};
			expect(client->validatePolicy(policy),
			       "hot-reload client locator metadata matches exactly");
			expect(client->entry("Patterns::CAppInfoCache::GetOrAddAppData") != nullptr &&
			       !client->entry("Patterns::CAppInfoCache::GetOrAddAppData")->required,
			       "optional client function remains optional");
			expect(client->entry(
				"Patterns::CAppInfoCache::ThreadedReadFromDisk") != nullptr &&
			       !client->entry(
				"Patterns::CAppInfoCache::ThreadedReadFromDisk")->required,
			       "optional live disk reload remains optional");
			expect(client->entry("Patterns::CAppInfoCache::SkipFlagReference") != nullptr &&
			       client->entry("Patterns::CAppInfoCache::SkipFlagReference")->targetRva == 5100,
			       "instruction-site lookup keeps its exact RVA");
		}
	}
	{
		const auto body = steamUiToml();
		auto ui = PatternCatalog::parseCanonical(body, steamUiExpected, &error);
		expect(ui.has_value(), "exact SteamUI catalog parses in the SteamUI module");
		if (ui)
		{
			const PatternCatalog::CompiledLocator policy[] =
			{
				{"Patterns::SteamUI::AppControllerRunFrame", false,
				 "55 57 56 53 E8 ? ? ? ?", "None"},
			};
			expect(ui->validatePolicy(policy),
			       "SteamUI locator is independently addressable and optional");
		}
		auto wrongModule = PatternCatalog::parseCanonical(body, expected, &error);
		expect(!wrongModule,
		       "a SteamUI catalog cannot supply a steamclient locator");
	}

	{
		auto body = validToml();
		replaceOnce(body, "schema = 1", "schema = 2");
		expectRejected(body, "wrong schema is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "component = \"steamclient\"", "component = \"steamui\"");
		expectRejected(body, "wrong component is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "module_size = 8192", "module_size = 8193");
		expectRejected(body, "wrong module identity is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "target_rva = 4660", "target_rva = 4294967296");
		expectRejected(body, "RVA overflow is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "signature = \"55 89 E5\"", "signature = \"55 89 e5\"");
		expectRejected(body, "noncanonical lowercase signature bytes are rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "target_rva = 4660\n", "");
		expectRejected(body, "missing required locator value is rejected");
	}
	{
		auto body = validToml();
		const auto locator = body.substr(body.find("\n[[locators]]"));
		body += locator;
		expectRejected(body, "duplicate locator symbol is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "name = \"Root\"", "name = \"Root\\nInjected\"");
		expectRejected(body, "string escapes are rejected");
	}
	{
		auto body = validToml();
		body.insert(body.find("platform"), "unknown = 1\n");
		expectRejected(body, "unknown top-level field is rejected");
	}
	{
		auto body = validToml();
		body.insert(body.find("platform"), "schema = 1\n");
		expectRejected(body, "duplicate top-level field is rejected");
	}
	{
		auto body = validToml();
		body.insert(body.end() - 1, static_cast<char>(0xFF));
		expectRejected(body, "malformed UTF-8 is rejected");
	}
	{
		auto body = validToml();
		replaceOnce(body, "target_rva = 4660", "target_rva = 8192");
		expectRejected(body, "RVA outside the declared module is rejected");
	}
	{
		auto body = validToml();
		body += "trailing = 1\n";
		expectRejected(body, "trailing garbage is rejected");
	}

	if (failures != 0)
	{
		std::cerr << failures << " pattern catalog test(s) failed\n";
		return 1;
	}
	std::cout << "pattern catalog tests passed\n";
	return 0;
}
