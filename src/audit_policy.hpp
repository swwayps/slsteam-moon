#pragma once

#include <cstring>
#include <link.h>

namespace AuditBinding
{
	inline bool hasSuffix(const char* value, const char* suffix) noexcept
	{
		if (value == nullptr || suffix == nullptr)
			return false;
		const std::size_t valueLength = std::strlen(value);
		const std::size_t suffixLength = std::strlen(suffix);
		return valueLength >= suffixLength
			&& std::strcmp(value + valueLength - suffixLength, suffix) == 0;
	}

	inline bool isBindingSource(const char* objectName) noexcept
	{
		if (objectName == nullptr)
			return false;
		// glibc reports the main executable's link-map name as an empty string.
		return objectName[0] == '\0'
			|| hasSuffix(objectName, "/steam")
			|| hasSuffix(objectName, "/crashhandler.so")
			|| hasSuffix(objectName, "/libglib-2.0.so.0")
			|| hasSuffix(objectName, "/libtier0_s.so")
			|| hasSuffix(objectName, "/steamclient.so")
			|| hasSuffix(objectName, "/steamservice.so")
			|| hasSuffix(objectName, "/libSDL3.so.0")
			|| hasSuffix(objectName, "/libvideo.so")
			|| hasSuffix(objectName, "/gameoverlayrenderer.so")
			|| hasSuffix(objectName, "/steam_monitor")
			|| hasSuffix(objectName, "/steamui.so")
			|| hasSuffix(objectName, "/cloud_redirect.so");
	}

	inline bool isBindingTarget(const char* objectName) noexcept
	{
		return hasSuffix(objectName, "/libc.so.6")
			|| hasSuffix(objectName, "/libpthread.so.0")
			|| hasSuffix(objectName, "/cloud_redirect.so");
	}

	inline bool bindAllValueEnabled(const char* value) noexcept
	{
		return value != nullptr && std::strcmp(value, "1") == 0;
	}

	inline bool narrowValueEnabled(const char* value) noexcept
	{
		return bindAllValueEnabled(value);
	}

	inline unsigned int flagsForObject(const char* objectName, bool bindAll) noexcept
	{
		if (bindAll)
			return LA_FLG_BINDFROM | LA_FLG_BINDTO;

		unsigned int flags = 0;
		if (isBindingSource(objectName))
			flags |= LA_FLG_BINDFROM;
		if (isBindingTarget(objectName))
			flags |= LA_FLG_BINDTO;
		return flags;
	}
}
