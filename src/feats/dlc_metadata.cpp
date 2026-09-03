// SPDX-License-Identifier: AGPL-3.0-only

#include "dlc_metadata.hpp"

#include "dlcids.hpp"

#include "../ascii.hpp"

#include "yaml-cpp/yaml.h"
#include "yaml-cpp/emitter.h"

#include "base64/base64.hpp"

#include <charconv>
#include <string>

namespace DlcMetadata
{
namespace
{
class VdfReader
{
public:
	VdfReader(const char* begin, const char* end) : current_(begin), end_(end) {}

	YAML::Node parseAppinfo()
	{
		std::string token;
		if (next(token) != Token::String || token != "appinfo")
			return YAML::Node(YAML::NodeType::Undefined);
		if (next(token) != Token::OpenBrace)
			return YAML::Node(YAML::NodeType::Undefined);
		bool closed = false;
		YAML::Node result = parseObject(closed);
		if (!closed) return YAML::Node(YAML::NodeType::Undefined);
		if (next(token) != Token::End)
			return YAML::Node(YAML::NodeType::Undefined);
		return result;
	}

private:
	enum class Token { String, OpenBrace, CloseBrace, End, Invalid };

	YAML::Node parseObject(bool& closed)
	{
		closed = false;
		YAML::Node node(YAML::NodeType::Map);
		std::string key;
		for (;;)
		{
			const Token keyToken = next(key);
			if (keyToken == Token::CloseBrace)
			{
				closed = true;
				return node;
			}
			if (keyToken != Token::String)
				return YAML::Node(YAML::NodeType::Undefined);

			std::string value;
			const Token valueToken = next(value);
			if (valueToken == Token::String)
			{
				node[key] = value;
				continue;
			}
			if (valueToken != Token::OpenBrace)
				return YAML::Node(YAML::NodeType::Undefined);

			bool childClosed = false;
			YAML::Node child = parseObject(childClosed);
			if (!childClosed || !child)
				return YAML::Node(YAML::NodeType::Undefined);
			node[key] = child;
		}
	}

	Token next(std::string& output)
	{
		output.clear();
		skipWhitespace();
		if (current_ >= end_) return Token::End;
		if (*current_ == '{') { ++current_; return Token::OpenBrace; }
		if (*current_ == '}') { ++current_; return Token::CloseBrace; }
		if (*current_ == '"') return quoted(output);
		return bare(output);
	}

	void skipWhitespace()
	{
		while (current_ < end_)
		{
			const unsigned char value = static_cast<unsigned char>(*current_);
			if (Ascii::isSpace(value)) { ++current_; continue; }
			if (*current_ == '/' && current_ + 1 < end_ && current_[1] == '/')
			{
				while (current_ < end_ && *current_ != '\n') ++current_;
				continue;
			}
			break;
		}
	}

	Token quoted(std::string& output)
	{
		++current_;
		while (current_ < end_)
		{
			const char value = *current_++;
			if (value == '"') return Token::String;
			if (value == '\\' && current_ < end_)
			{
				const char escaped = *current_++;
				switch (escaped)
				{
					case 'n': output.push_back('\n'); break;
					case 'r': output.push_back('\r'); break;
					case 't': output.push_back('\t'); break;
					default: output.push_back(escaped); break;
				}
				continue;
			}
			output.push_back(value);
		}
		return Token::Invalid;
	}

	Token bare(std::string& output)
	{
		while (current_ < end_)
		{
			const unsigned char value = static_cast<unsigned char>(*current_);
			if (Ascii::isSpace(value) || *current_ == '{' || *current_ == '}' ||
				*current_ == '"') break;
			output.push_back(*current_++);
		}
		return output.empty() ? Token::Invalid : Token::String;
	}

	const char* current_;
	const char* end_;
};

bool parseAppId(const YAML::Node& node, std::uint32_t& output)
{
	output = 0;
	if (!node || !node.IsScalar()) return false;
	const std::string value = node.as<std::string>("");
	const auto parsed = std::from_chars(
		value.data(), value.data() + value.size(), output);
	return parsed.ec == std::errc{} &&
		parsed.ptr == value.data() + value.size() && output != 0;
}

void emitEscaped(std::string& output, const std::string& value)
{
	output.push_back('"');
	for (const char character : value)
	{
		if (character == '\\' || character == '"') output.push_back('\\');
		output.push_back(character);
	}
	output.push_back('"');
}

void emitMap(std::string& output, const YAML::Node& node, int depth)
{
	for (auto item = node.begin(); item != node.end(); ++item)
	{
		if (!item->first.IsScalar()) continue;
		const std::string key = item->first.as<std::string>();
		const YAML::Node value = item->second;
		output.append(static_cast<std::size_t>(depth), '\t');
		emitEscaped(output, key);
		if (value.IsMap())
		{
			output.append("\n");
			output.append(static_cast<std::size_t>(depth), '\t');
			output.append("{\n");
			emitMap(output, value, depth + 1);
			output.append(static_cast<std::size_t>(depth), '\t');
			output.append("}\n");
		}
		else if (value.IsScalar())
		{
			output.append("\t\t");
			emitEscaped(output, value.as<std::string>(""));
			output.push_back('\n');
		}
		else
		{
			output.append("\t\t\"\"\n");
		}
	}
}

std::string lower(std::string value)
{
	for (char& character : value)
		character = static_cast<char>(
			Ascii::toLower(static_cast<unsigned char>(character)));
	return value;
}
} // namespace

bool normalize(
	const std::string& cmWire,
	std::uint32_t requestedDlcAppId,
	std::uint32_t baseAppId,
	std::string& normalizedWire) noexcept
{
	normalizedWire.clear();
	if (cmWire.empty() || requestedDlcAppId == 0 || baseAppId == 0 ||
		requestedDlcAppId == baseAppId)
		return false;

	try
	{
		std::size_t wireSize = cmWire.size();
		while (wireSize != 0)
		{
			const unsigned char trailing =
				static_cast<unsigned char>(cmWire[wireSize - 1]);
			if (trailing != 0 && !Ascii::isSpace(trailing)) break;
			--wireSize;
		}
		if (wireSize == 0) return false;
		VdfReader reader(cmWire.data(), cmWire.data() + wireSize);
		const YAML::Node body = reader.parseAppinfo();
		if (!body || !body.IsMap()) return false;

		std::uint32_t wireAppId = 0;
		if (!parseAppId(body["appid"], wireAppId)) return false;

		const YAML::Node common = body["common"];
		if (!common || !common.IsMap()) return false;
		const YAML::Node type = common["type"];
		if (!type || !type.IsScalar() ||
			lower(type.as<std::string>("")) != "dlc") return false;

		std::uint32_t commonParent = 0;
		std::uint32_t extendedParent = 0;
		const bool hasCommonParent = parseAppId(common["parent"], commonParent);
		const YAML::Node extended = body["extended"];
		const bool hasExtendedParent = extended && extended.IsMap() &&
			parseAppId(extended["dlcforappid"], extendedParent);
		if (!hasCommonParent && !hasExtendedParent) return false;
		if (hasCommonParent && hasExtendedParent &&
			commonParent != extendedParent) return false;
		const std::uint32_t parent = hasCommonParent
			? commonParent : extendedParent;

		const AppInfoProvision::DlcMetadataFacts facts{
			.requestedBaseAppId = baseAppId,
			.requestedDlcAppId = requestedDlcAppId,
			.wireAppId = wireAppId,
			.parentAppId = parent,
			.hasCommon = true,
			.typeIsDlc = true,
		};
		if (!AppInfoProvision::isValidDlcMetadata(facts)) return false;

		YAML::Node safe(YAML::NodeType::Map);
		safe["appid"] = body["appid"];
		safe["common"] = YAML::Clone(common);
		if (extended && extended.IsMap())
			safe["extended"] = YAML::Clone(extended);

		normalizedWire = "\"appinfo\"\n{\n";
		emitMap(normalizedWire, safe, 1);
		normalizedWire.append("}\n");
		return true;
	}
	catch (...)
	{
		normalizedWire.clear();
		return false;
	}
}

bool encodeCache(const CacheRecord& record, std::string& output) noexcept
{
	output.clear();
	if (record.baseAppId == 0 || record.baseSha.size() != 20) return false;
	try
	{
		YAML::Emitter emitter;
		emitter << YAML::BeginMap
			<< YAML::Key << "schema" << YAML::Value << 1
			<< YAML::Key << "base_appid" << YAML::Value << record.baseAppId
			<< YAML::Key << "base_generation" << YAML::Value
			<< record.baseGeneration
			<< YAML::Key << "base_change_number" << YAML::Value
			<< record.baseChangeNumber
			<< YAML::Key << "base_sha_b64" << YAML::Value
			<< base64::to_base64(record.baseSha)
			<< YAML::Key << "apps" << YAML::Value << YAML::BeginSeq;
		for (const MetadataApp& app : record.apps)
		{
			if (app.appid == 0 || app.sha.size() != 20 ||
				app.wireBuffer.empty()) return false;
			emitter << YAML::BeginMap
				<< YAML::Key << "appid" << YAML::Value << app.appid
				<< YAML::Key << "change_number" << YAML::Value
				<< app.changeNumber
				<< YAML::Key << "sha_b64" << YAML::Value
				<< base64::to_base64(app.sha)
				<< YAML::Key << "wire_b64" << YAML::Value
				<< base64::to_base64(app.wireBuffer)
				<< YAML::EndMap;
		}
		emitter << YAML::EndSeq
			<< YAML::Key << "rejected_appids" << YAML::Value << YAML::BeginSeq;
		for (const std::uint32_t appId : record.rejectedAppIds)
		{
			if (appId == 0 || appId == record.baseAppId) return false;
			emitter << appId;
		}
		emitter << YAML::EndSeq << YAML::EndMap;
		if (!emitter.good()) return false;
		output.assign(emitter.c_str(), emitter.size());
		return !output.empty();
	}
	catch (...)
	{
		output.clear();
		return false;
	}
}

bool decodeCache(const std::string& input, CacheRecord& record) noexcept
{
	record = {};
	if (input.empty() || input.size() > (32u << 20)) return false;
	try
	{
		const YAML::Node root = YAML::Load(input);
		if (!root.IsMap() || root["schema"].as<unsigned int>(0) != 1)
			return false;
		record.baseAppId = root["base_appid"].as<std::uint32_t>(0);
		record.baseGeneration = root["base_generation"].as<std::uint64_t>(0);
		record.baseChangeNumber =
			root["base_change_number"].as<std::uint32_t>(0);
		if (!root["base_sha_b64"]) return false;
		record.baseSha = std::string(base64::from_base64(
			root["base_sha_b64"].as<std::string>()));
		const YAML::Node apps = root["apps"];
		const YAML::Node rejected = root["rejected_appids"];
		if (record.baseAppId == 0 || record.baseSha.size() != 20 ||
			!apps || !apps.IsSequence() ||
			!rejected || !rejected.IsSequence() ||
			apps.size() + rejected.size() > 4096)
			return false;
		record.apps.reserve(apps.size());
		for (const YAML::Node& node : apps)
		{
			if (!node.IsMap() || !node["sha_b64"] || !node["wire_b64"])
				return false;
			MetadataApp app;
			app.appid = node["appid"].as<std::uint32_t>(0);
			app.changeNumber =
				node["change_number"].as<std::uint32_t>(0);
			app.sha = std::string(base64::from_base64(
				node["sha_b64"].as<std::string>()));
			app.wireBuffer = std::string(base64::from_base64(
				node["wire_b64"].as<std::string>()));
			if (app.appid == 0 || app.sha.size() != 20 ||
				app.wireBuffer.empty() || app.wireBuffer.size() > (4u << 20))
				return false;
			record.apps.push_back(std::move(app));
		}
		for (const YAML::Node& node : rejected)
		{
			const std::uint32_t appId = node.as<std::uint32_t>(0);
			if (appId == 0 || appId == record.baseAppId) return false;
			record.rejectedAppIds.push_back(appId);
		}
		return true;
	}
	catch (...)
	{
		record = {};
		return false;
	}
}
} // namespace DlcMetadata
