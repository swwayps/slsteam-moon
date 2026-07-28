#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace AppTicket
{
	inline constexpr uint32_t kSteamIdOffset = 8;
	inline constexpr uint32_t kExplicitAppIdOffset = 16;
	inline constexpr uint32_t kSignatureSize = 128;
	inline constexpr uint32_t kLocalSourceAppId = 7;

	enum class Source
	{
		None,
		Explicit,
		LocalDerived,
	};

	struct PreparedOwnershipTicket
	{
		Source source = Source::None;
		std::vector<uint8_t> data;
		uint32_t totalSize = 0;
		uint32_t appIdOffset = 0;
		uint32_t steamIdOffset = 0;
		uint32_t signatureOffset = 0;
		uint32_t signatureSize = 0;

		explicit operator bool() const
		{
			return source != Source::None && !data.empty();
		}
	};

	namespace Detail
	{
		inline uint32_t read32(std::span<const uint8_t> bytes, std::size_t offset)
		{
			uint32_t value = 0;
			std::memcpy(&value, bytes.data() + offset, sizeof(value));
			return value;
		}

		inline bool signedTicketShape(
			std::span<const uint8_t> bytes,
			uint32_t& signatureOffset)
		{
			if (bytes.size() < kSignatureSize + kExplicitAppIdOffset + sizeof(uint32_t))
			{
				return false;
			}

			signatureOffset = read32(bytes, 0);
			if (signatureOffset < kExplicitAppIdOffset + sizeof(uint32_t))
			{
				return false;
			}

			return static_cast<std::size_t>(signatureOffset) + kSignatureSize
			    == bytes.size();
		}
	}

	inline PreparedOwnershipTicket explicitOwnershipTicket(
		std::span<const uint8_t> bytes)
	{
		uint32_t signatureOffset = 0;
		if (!Detail::signedTicketShape(bytes, signatureOffset))
		{
			return {};
		}

		PreparedOwnershipTicket result;
		result.source = Source::Explicit;
		result.data.assign(bytes.begin(), bytes.end());
		result.totalSize = static_cast<uint32_t>(bytes.size());
		result.appIdOffset = kExplicitAppIdOffset;
		result.steamIdOffset = kSteamIdOffset;
		result.signatureOffset = signatureOffset;
		result.signatureSize = kSignatureSize;
		return result;
	}

	inline bool isLocalSourceTicket(std::span<const uint8_t> bytes)
	{
		uint32_t signatureOffset = 0;
		return Detail::signedTicketShape(bytes, signatureOffset)
		    && Detail::read32(bytes, kExplicitAppIdOffset) == kLocalSourceAppId;
	}

	inline PreparedOwnershipTicket deriveLocalOwnershipTicket(
		std::span<const uint8_t> source,
		uint32_t appId)
	{
		uint32_t sourceSignatureOffset = 0;
		if (!appId || !Detail::signedTicketShape(source, sourceSignatureOffset)
		    || Detail::read32(source, kExplicitAppIdOffset) != kLocalSourceAppId)
		{
			return {};
		}

		PreparedOwnershipTicket result;
		result.source = Source::LocalDerived;
		result.data.reserve(source.size() + sizeof(appId));
		result.data.insert(
			result.data.end(), source.begin(), source.begin() + sourceSignatureOffset);

		const auto* appIdBytes = reinterpret_cast<const uint8_t*>(&appId);
		result.data.insert(
			result.data.end(), appIdBytes, appIdBytes + sizeof(appId));
		result.data.insert(
			result.data.end(), source.begin() + sourceSignatureOffset, source.end());

		// The client-side parser consumes one field beyond the reported logical
		// boundary. Keep the original logical size while retaining the inserted
		// AppID in the physical buffer immediately before the signature.
		result.totalSize = static_cast<uint32_t>(source.size());
		result.appIdOffset = sourceSignatureOffset;
		result.steamIdOffset = kSteamIdOffset;
		result.signatureOffset = sourceSignatureOffset + sizeof(appId);
		result.signatureSize = kSignatureSize;
		return result;
	}

	inline PreparedOwnershipTicket prepareOwnershipTicket(
		std::span<const uint8_t> explicitBytes,
		std::span<const uint8_t> localSource,
		uint32_t appId)
	{
		if (auto explicitTicket = explicitOwnershipTicket(explicitBytes);
		    explicitTicket
		    && Detail::read32(explicitBytes, kExplicitAppIdOffset) == appId)
		{
			return explicitTicket;
		}
		return deriveLocalOwnershipTicket(localSource, appId);
	}

	inline bool copyOwnershipTicket(
		const PreparedOwnershipTicket& ticket,
		void* destination,
		std::size_t capacity,
		uint32_t* appIdOffset,
		uint32_t* steamIdOffset,
		uint32_t* signatureOffset,
		uint32_t* signatureSize)
	{
		if (!ticket || !destination || capacity < ticket.data.size()
		    || !appIdOffset || !steamIdOffset || !signatureOffset || !signatureSize)
		{
			return false;
		}

		std::memcpy(destination, ticket.data.data(), ticket.data.size());
		*appIdOffset = ticket.appIdOffset;
		*steamIdOffset = ticket.steamIdOffset;
		*signatureOffset = ticket.signatureOffset;
		*signatureSize = ticket.signatureSize;
		return true;
	}
}
