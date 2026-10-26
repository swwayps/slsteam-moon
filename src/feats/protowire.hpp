// SPDX-License-Identifier: AGPL-3.0-only
//
// Raw protobuf wire walking for the messages we edit without generated
// classes. Pure and bounds-checked: nothing here reads past `len`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ProtoWire
{

// Read a base-128 varint at `pos` (advancing it). Returns false on overrun
// or an over-long (>10 byte) varint.
inline bool readVarint(const uint8_t* data, std::size_t len,
	                   std::size_t& pos, uint64_t& out)
{
	uint64_t value = 0;
	int shift = 0;
	while (pos < len && shift <= 63)
	{
		const uint8_t byte = data[pos++];
		if (shift == 63 && (byte & 0xfe) != 0) return false;
		value |= static_cast<uint64_t>(byte & 0x7f) << shift;
		if (!(byte & 0x80))
		{
			out = value;
			return true;
		}
		shift += 7;
	}
	return false;
}

// One top-level field located on the wire.
struct Field
{
	uint32_t number;
	uint8_t wireType;
	std::size_t start;     // offset of the tag byte
	std::size_t end;       // one past the field's last byte
	std::size_t valueOff;  // offset of the value (after tag, after len for wt2)
	std::size_t valueLen;  // value length in bytes (wt0: the varint's bytes)
};

// Walk the top-level fields of a message, calling `fn(Field)` for each.
// Returns false when the buffer is malformed (a field number outside
// 1..2^29-1, groups, unknown wire types, overruns); the caller decides what
// to do with the fields seen so far.
template<typename Fn>
inline bool walk(const uint8_t* data, std::size_t len, Fn&& fn)
{
	std::size_t pos = 0;
	while (pos < len)
	{
		const std::size_t start = pos;
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag)) return false;

		const uint64_t number64 = tag >> 3;
		if (number64 == 0 || number64 > 0x1fffffffULL) return false;
		const uint32_t number = static_cast<uint32_t>(number64);
		const uint8_t wireType = static_cast<uint8_t>(tag & 7);

		std::size_t valueOff = pos;
		std::size_t valueLen = 0;
		switch (wireType)
		{
		case 0:
		{
			const std::size_t valueStart = pos;
			uint64_t ignored = 0;
			if (!readVarint(data, len, pos, ignored)) return false;
			valueOff = valueStart;
			valueLen = pos - valueStart;
			break;
		}
		case 1:
			if (len - pos < 8) return false;
			valueOff = pos;
			valueLen = 8;
			pos += 8;
			break;
		case 2:
		{
			uint64_t size = 0;
			if (!readVarint(data, len, pos, size)) return false;
			if (size > len - pos) return false;
			valueOff = pos;
			valueLen = static_cast<std::size_t>(size);
			pos += valueLen;
			break;
		}
		case 5:
			if (len - pos < 4) return false;
			valueOff = pos;
			valueLen = 4;
			pos += 4;
			break;
		default:
			return false;
		}

		fn(Field{ number, wireType, start, pos, valueOff, valueLen });
	}
	return true;
}

inline void putVarint(std::vector<uint8_t>& out, uint64_t value)
{
	while (value >= 0x80)
	{
		out.push_back(static_cast<uint8_t>(value) | 0x80);
		value >>= 7;
	}
	out.push_back(static_cast<uint8_t>(value));
}

inline void putVarintField(std::vector<uint8_t>& out,
	                       uint32_t number, uint64_t value)
{
	putVarint(out, static_cast<uint64_t>(number) << 3);
	putVarint(out, value);
}

} // namespace ProtoWire
