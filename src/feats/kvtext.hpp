// KV1 text, the format of a PICS appinfo buffer. The provisioner, the
// appinfo.vdf splice and the DLC normalizer read and write it through this
// one lexer, reader and writer, so they accept and refuse the same input.
#pragma once

#include "../ascii.hpp"

#include "yaml-cpp/yaml.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace KvText
{

// Quoted or bare strings, braces and `//` comments. An unterminated quoted
// string is Invalid.
class Lexer
{
public:
	enum class Tok { String, OpenBrace, CloseBrace, End, Invalid };

	explicit Lexer(std::string_view text)
		: p_(text.data()), end_(text.data() + text.size()) {}

	Tok next(std::string& out)
	{
		out.clear();
		skipSpace();
		if (p_ >= end_) return Tok::End;
		if (*p_ == '{') { ++p_; return Tok::OpenBrace; }
		if (*p_ == '}') { ++p_; return Tok::CloseBrace; }
		if (*p_ == '"') return quoted(out);
		return bare(out);
	}

private:
	void skipSpace()
	{
		while (p_ < end_)
		{
			if (Ascii::isSpace(static_cast<unsigned char>(*p_))) { ++p_; continue; }
			if (*p_ == '/' && p_ + 1 < end_ && p_[1] == '/')
			{
				while (p_ < end_ && *p_ != '\n') ++p_;
				continue;
			}
			break;
		}
	}

	Tok quoted(std::string& out)
	{
		++p_;
		while (p_ < end_)
		{
			const char c = *p_++;
			if (c == '"') return Tok::String;
			if (c == '\\' && p_ < end_)
			{
				const char escaped = *p_++;
				switch (escaped)
				{
					case 'n': out.push_back('\n'); break;
					case 't': out.push_back('\t'); break;
					case 'r': out.push_back('\r'); break;
					default: out.push_back(escaped); break;
				}
				continue;
			}
			out.push_back(c);
		}
		return Tok::Invalid;
	}

	Tok bare(std::string& out)
	{
		while (p_ < end_ && !Ascii::isSpace(static_cast<unsigned char>(*p_)) &&
		       *p_ != '{' && *p_ != '}' && *p_ != '"')
			out.push_back(*p_++);
		return out.empty() ? Tok::Invalid : Tok::String;
	}

	const char* p_;
	const char* end_;
};

// The text without the NULs and whitespace a PICS buffer ends with.
inline std::string_view trimmed(std::string_view text)
{
	while (!text.empty() && (text.back() == '\0' ||
	                         Ascii::isSpace(static_cast<unsigned char>(text.back()))))
		text.remove_suffix(1);
	return text;
}

namespace detail
{
// The pairs of an object whose opening brace was read; undefined unless its
// closing brace follows.
inline YAML::Node parseObject(Lexer& lexer)
{
	YAML::Node node(YAML::NodeType::Map);
	std::string key, value;
	for (;;)
	{
		const Lexer::Tok keyTok = lexer.next(key);
		if (keyTok == Lexer::Tok::CloseBrace) return node;
		if (keyTok != Lexer::Tok::String) return YAML::Node(YAML::NodeType::Undefined);
		const Lexer::Tok valueTok = lexer.next(value);
		if (valueTok == Lexer::Tok::String)
		{
			node[key] = value;
			continue;
		}
		if (valueTok != Lexer::Tok::OpenBrace) return YAML::Node(YAML::NodeType::Undefined);
		YAML::Node child = parseObject(lexer);
		if (!child) return child;
		node[key] = child;
	}
}

inline void emitEscaped(std::string& out, const std::string& text)
{
	out.push_back('"');
	for (const char c : text)
	{
		if (c == '\\' || c == '"') out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
}
} // namespace detail

// The body of `"appinfo" { ... }`, or an undefined node for anything else:
// another top key, an unterminated object or string, or a token after the
// closing brace.
inline YAML::Node parseAppinfo(std::string_view text)
{
	Lexer lexer(trimmed(text));
	std::string token;
	if (lexer.next(token) != Lexer::Tok::String || token != "appinfo" ||
	    lexer.next(token) != Lexer::Tok::OpenBrace)
		return YAML::Node(YAML::NodeType::Undefined);
	YAML::Node body = detail::parseObject(lexer);
	if (!body || lexer.next(token) != Lexer::Tok::End)
		return YAML::Node(YAML::NodeType::Undefined);
	return body;
}

// The pairs of `node` at `depth` tabs. A sequence becomes an object indexed
// "0", "1", ...; any other value that is not a scalar becomes "".
inline void emitMap(std::string& out, const YAML::Node& node, int depth)
{
	const auto emitValue = [&out](const YAML::Node& value, int at)
	{
		if (value.IsScalar())
		{
			out.append("\t\t");
			detail::emitEscaped(out, value.as<std::string>(""));
			out.push_back('\n');
			return;
		}
		if (!value.IsMap() && !value.IsSequence())
		{
			out.append("\t\t\"\"\n");
			return;
		}
		out.push_back('\n');
		out.append(static_cast<std::size_t>(at), '\t');
		out.append("{\n");
		if (value.IsMap())
		{
			emitMap(out, value, at + 1);
		}
		else
		{
			YAML::Node indexed(YAML::NodeType::Map);
			int index = 0;
			for (const YAML::Node& child : value) indexed[std::to_string(index++)] = child;
			emitMap(out, indexed, at + 1);
		}
		out.append(static_cast<std::size_t>(at), '\t');
		out.append("}\n");
	};
	for (auto it = node.begin(); it != node.end(); ++it)
	{
		if (!it->first.IsScalar()) continue;
		out.append(static_cast<std::size_t>(depth), '\t');
		detail::emitEscaped(out, it->first.as<std::string>());
		emitValue(it->second, depth);
	}
}

// `"appinfo" { <body> }`.
inline std::string emitAppinfo(const YAML::Node& body)
{
	std::string out = "\"appinfo\"\n{\n";
	emitMap(out, body, 1);
	out.append("}\n");
	return out;
}

} // namespace KvText
