#include "manifest_zip.hpp"

#include <algorithm>
#include <cstdint>
#include <dlfcn.h>
#include <limits>

namespace
{
	using Byte = unsigned char;
	using UInt = unsigned int;
	using ULong = unsigned long;
	using VoidPtr = void*;
	using AllocFunction = VoidPtr (*)(VoidPtr, UInt, UInt);
	using FreeFunction = void (*)(VoidPtr, VoidPtr);
	struct InternalState;
	struct ZStream
	{
		Byte* nextIn = nullptr;
		UInt availableIn = 0;
		ULong totalIn = 0;
		Byte* nextOut = nullptr;
		UInt availableOut = 0;
		ULong totalOut = 0;
		char* message = nullptr;
		InternalState* state = nullptr;
		AllocFunction allocate = nullptr;
		FreeFunction release = nullptr;
		VoidPtr opaque = nullptr;
		int dataType = 0;
		ULong adler = 0;
		ULong reserved = 0;
	};

	using InflateInit2 = int (*)(ZStream*, int, const char*, int);
	using Inflate = int (*)(ZStream*, int);
	using InflateEnd = int (*)(ZStream*);
	using Crc32 = ULong (*)(ULong, const Byte*, UInt);
	using ZlibVersion = const char* (*)();

	struct ZlibApi
	{
		InflateInit2 initialize = nullptr;
		Inflate inflate = nullptr;
		InflateEnd finish = nullptr;
		Crc32 crc32 = nullptr;
		const char* version = nullptr;

		explicit operator bool() const noexcept
		{
			return initialize != nullptr && inflate != nullptr && finish != nullptr
			    && crc32 != nullptr && version != nullptr;
		}
	};

	const ZlibApi& zlibApi()
	{
		static const ZlibApi api = []
		{
			ZlibApi result;
			void* handle = dlopen("libz.so.1", RTLD_LAZY | RTLD_LOCAL);
			if (handle == nullptr) handle = dlopen("libz.so", RTLD_LAZY | RTLD_LOCAL);
			if (handle == nullptr) return result;
			result.initialize = reinterpret_cast<InflateInit2>(
				dlsym(handle, "inflateInit2_"));
			result.inflate = reinterpret_cast<Inflate>(dlsym(handle, "inflate"));
			result.finish = reinterpret_cast<InflateEnd>(dlsym(handle, "inflateEnd"));
			result.crc32 = reinterpret_cast<Crc32>(dlsym(handle, "crc32"));
			const auto version = reinterpret_cast<ZlibVersion>(
				dlsym(handle, "zlibVersion"));
			if (version != nullptr) result.version = version();
			return result;
		}();
		return api;
	}

	constexpr int kZOk = 0;
	constexpr int kZStreamEnd = 1;
	constexpr int kZFinish = 4;
	constexpr int kRawDeflateWindowBits = -15;
	constexpr std::uint32_t kLocalHeader = 0x04034b50u;
	constexpr std::uint32_t kCentralHeader = 0x02014b50u;
	constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50u;
	constexpr std::size_t kMaximumPayloadSize = 64u * 1024u * 1024u;
	constexpr std::size_t kMaximumEocdSearch = 65535u + 22u;

	void setDiagnostic(std::string* diagnostic, const char* message)
	{
		if (diagnostic != nullptr) *diagnostic = message;
	}

	bool rangeFits(std::size_t offset, std::size_t length,
	               std::size_t available) noexcept
	{
		return offset <= available && length <= available - offset;
	}

	std::uint16_t read16(std::string_view bytes, std::size_t offset)
	{
		const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
		return static_cast<std::uint16_t>(p[0])
		     | static_cast<std::uint16_t>(p[1]) << 8;
	}

	std::uint32_t read32(std::string_view bytes, std::size_t offset)
	{
		const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
		return static_cast<std::uint32_t>(p[0])
		     | static_cast<std::uint32_t>(p[1]) << 8
		     | static_cast<std::uint32_t>(p[2]) << 16
		     | static_cast<std::uint32_t>(p[3]) << 24;
	}

	std::size_t findEndOfCentralDirectory(std::string_view archive)
	{
		if (archive.size() < 22) return std::string_view::npos;
		const std::size_t begin = archive.size() > kMaximumEocdSearch
		                        ? archive.size() - kMaximumEocdSearch : 0;
		for (std::size_t offset = archive.size() - 22;; --offset)
		{
			if (read32(archive, offset) == kEndOfCentralDirectory)
				return offset;
			if (offset == begin) break;
		}
		return std::string_view::npos;
	}

	bool inflatePayload(std::string_view compressed, std::size_t expectedSize,
	                    std::vector<unsigned char>& output)
	{
		const auto& api = zlibApi();
		if (!api) return false;
		output.assign(expectedSize, 0);
		ZStream stream{};
		stream.nextIn = reinterpret_cast<Byte*>(
			const_cast<char*>(compressed.data()));
		stream.availableIn = static_cast<UInt>(compressed.size());
		stream.nextOut = output.data();
		stream.availableOut = static_cast<UInt>(output.size());
		if (api.initialize(&stream, kRawDeflateWindowBits, api.version,
		                   sizeof(stream)) != kZOk)
			return false;
		const int result = api.inflate(&stream, kZFinish);
		const bool complete = result == kZStreamEnd
		                   && stream.totalIn == compressed.size()
		                   && stream.totalOut == expectedSize;
		api.finish(&stream);
		if (!complete) output.clear();
		return complete;
	}
}


bool ManifestZip::extractSingleFile(std::string_view archive,
	                                 std::vector<unsigned char>& output,
	                                 std::string* diagnostic)
{
	output.clear();
	if (diagnostic != nullptr) diagnostic->clear();
	const std::size_t eocd = findEndOfCentralDirectory(archive);
	if (eocd == std::string_view::npos || !rangeFits(eocd, 22, archive.size()))
	{
		setDiagnostic(diagnostic, "end-of-central-directory record is missing");
		return false;
	}
	const std::size_t commentSize = read16(archive, eocd + 20);
	if (!rangeFits(eocd + 22, commentSize, archive.size())
	    || eocd + 22 + commentSize != archive.size())
	{
		setDiagnostic(diagnostic, "end-of-central-directory record is truncated");
		return false;
	}
	if (read16(archive, eocd + 4) != 0 || read16(archive, eocd + 6) != 0
	    || read16(archive, eocd + 8) != 1 || read16(archive, eocd + 10) != 1)
	{
		setDiagnostic(diagnostic, "archive is not a single-entry non-spanned ZIP");
		return false;
	}

	const std::size_t centralSize = read32(archive, eocd + 12);
	const std::size_t centralOffset = read32(archive, eocd + 16);
	if (!rangeFits(centralOffset, centralSize, eocd)
	    || centralOffset + centralSize != eocd
	    || centralSize < 46
	    || read32(archive, centralOffset) != kCentralHeader)
	{
		setDiagnostic(diagnostic, "central-directory entry is invalid");
		return false;
	}

	const std::uint16_t flags = read16(archive, centralOffset + 8);
	const std::uint16_t method = read16(archive, centralOffset + 10);
	const std::uint32_t expectedCrc = read32(archive, centralOffset + 16);
	const std::size_t compressedSize = read32(archive, centralOffset + 20);
	const std::size_t payloadSize = read32(archive, centralOffset + 24);
	const std::size_t centralNameSize = read16(archive, centralOffset + 28);
	const std::size_t centralExtraSize = read16(archive, centralOffset + 30);
	const std::size_t centralCommentSize = read16(archive, centralOffset + 32);
	const std::size_t localOffset = read32(archive, centralOffset + 42);
	const std::size_t centralEntrySize = 46 + centralNameSize
	                                   + centralExtraSize + centralCommentSize;
	if (centralEntrySize != centralSize
	    || (flags & 0x0009u) != 0
	    || (method != 0 && method != 8)
	    || payloadSize == 0 || payloadSize > kMaximumPayloadSize
	    || compressedSize > kMaximumPayloadSize
	    || !rangeFits(localOffset, 30, centralOffset)
	    || read32(archive, localOffset) != kLocalHeader)
	{
		setDiagnostic(diagnostic, "ZIP entry policy or bounds check failed");
		return false;
	}

	const std::uint16_t localFlags = read16(archive, localOffset + 6);
	const std::uint16_t localMethod = read16(archive, localOffset + 8);
	const std::uint32_t localCrc = read32(archive, localOffset + 14);
	const std::size_t localCompressedSize = read32(archive, localOffset + 18);
	const std::size_t localPayloadSize = read32(archive, localOffset + 22);
	const std::size_t localNameSize = read16(archive, localOffset + 26);
	const std::size_t localExtraSize = read16(archive, localOffset + 28);
	const std::size_t localNameOffset = localOffset + 30;
	const std::size_t localVariableSize = localNameSize + localExtraSize;
	if (localFlags != flags || localMethod != method || localCrc != expectedCrc
	    || localCompressedSize != compressedSize || localPayloadSize != payloadSize
	    || localNameSize != centralNameSize
	    || !rangeFits(localNameOffset, localVariableSize, centralOffset)
	    || archive.substr(localOffset + 30, localNameSize)
	       != archive.substr(centralOffset + 46, centralNameSize))
	{
		setDiagnostic(diagnostic, "local and central ZIP metadata disagree");
		return false;
	}
	const std::size_t dataOffset = localNameOffset + localVariableSize;
	if (!rangeFits(dataOffset, compressedSize, centralOffset))
	{
		setDiagnostic(diagnostic, "compressed ZIP payload is truncated");
		return false;
	}

	const std::string_view compressed = archive.substr(dataOffset, compressedSize);
	const auto& api = zlibApi();
	if (!api)
	{
		setDiagnostic(diagnostic, "zlib runtime is unavailable");
		return false;
	}
	if (method == 0)
	{
		if (compressedSize != payloadSize)
		{
			setDiagnostic(diagnostic, "stored ZIP entry has inconsistent sizes");
			return false;
		}
		output.assign(compressed.begin(), compressed.end());
	}
	else if (!inflatePayload(compressed, payloadSize, output))
	{
		setDiagnostic(diagnostic, "DEFLATE payload is invalid");
		return false;
	}

	const std::uint32_t actualCrc = static_cast<std::uint32_t>(
		api.crc32(0, output.data(), static_cast<UInt>(output.size())));
	if (actualCrc != expectedCrc)
	{
		output.clear();
		setDiagnostic(diagnostic, "manifest payload CRC32 mismatch");
		return false;
	}
	return true;
}
