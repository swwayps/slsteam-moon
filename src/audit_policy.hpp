#pragma once

#include <cstring>
#include <fcntl.h>
#include <link.h>
#include <span>
#include <unistd.h>

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

	// la_objsearch refuses these, as upstream abafc49 does: Steam's crash
	// handler writes minidumps that carry SLSsteam paths and $HOME and uploads
	// them to Valve.
	inline bool isRefusedObject(const char* objectName) noexcept
	{
		return hasSuffix(objectName, "crashhandler.so");
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

	// Host directories holding 32-bit libraries, most specific first: on Arch
	// /usr/lib holds the 64-bit copies, on Fedora it is the 32-bit tree.
	inline constexpr const char* systemLibraryDirs[] =
	{
		"/usr/lib32",              // Arch, SteamOS
		"/usr/lib/i386-linux-gnu", // Debian, Ubuntu, Mint
		"/usr/local/lib32",        // Bazzite
		"/usr/lib",                // Fedora, 32-bit systems
		"/usr/local/lib",
	};

	inline bool isElf32(const char* path) noexcept
	{
		const int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return false;
		unsigned char ident[EI_NIDENT];
		const bool elf32 = read(fd, ident, sizeof ident) == static_cast<ssize_t>(sizeof ident)
			&& std::memcmp(ident, ELFMAG, SELFMAG) == 0
			&& ident[EI_CLASS] == ELFCLASS32;
		close(fd);
		return elf32;
	}

	// la_objsearch body. Steam's runtime pins its own old libcurl ahead of
	// the host copy, so a bare libcurl.so[.N] lookup gets the first 32-bit
	// host copy in dirs, built in buffer. Any other name, or one without a
	// host copy, is returned unchanged. Runs inside the loader: no
	// allocation, locale or logging.
	inline const char* systemLibraryFor(const char* name,
	                                    std::span<const char* const> dirs,
	                                    std::span<char> buffer) noexcept
	{
		if (name == nullptr || std::strncmp(name, "libcurl.so", 10) != 0
		    || (name[10] != '\0' && name[10] != '.'))
			return name;
		const std::size_t nameLength = std::strlen(name);
		for (const char* dir : dirs)
		{
			const std::size_t dirLength = std::strlen(dir);
			if (dirLength + 1 + nameLength >= buffer.size())
				continue;
			std::memcpy(buffer.data(), dir, dirLength);
			buffer[dirLength] = '/';
			std::memcpy(buffer.data() + dirLength + 1, name, nameLength + 1);
			if (isElf32(buffer.data()))
				return buffer.data();
		}
		return name;
	}
}
