#pragma once

// TLS CA-bundle discovery for libcurl.
//
// PROBLEM.  Steam ships its own libcurl inside the runtime; its compiled-in
// default CA path matches the build host, not the user's distro.  On
// SteamOS/Arch the real trust store lives at
// /etc/ca-certificates/extracted/tls-ca-bundle.pem and is only *symlinked*
// into /etc/ssl/certs — and in the runtime sandbox even that default may not
// resolve.  When it doesn't, every HTTPS call fails with curl error 60
// ("Peer certificate cannot be authenticated with given CA certificates"),
// which is exactly what sinks the native-CM list fetch (-> "empty CM list")
// and the steamcmd.net fallback (-> provisioning fails -> the user sees
// "Couldn't prepare a game's data").
//
// FIX.  Discover an existing trust store ONCE and pin every curl handle to it
// via CURLOPT_CAINFO (file) / CURLOPT_CAPATH (dir).  Discovery honors the
// usual env overrides first (so a user can always point us at the right file)
// then probes the well-known locations across distros.
//
// The core (discoverWith) is PURE — env-getter + path-exists predicate are
// injected — so it is unit-testable without the real filesystem
// (tools/test_cainfo.cpp).  bundleFile()/bundleDir() are the cached,
// real-filesystem wrappers the curl call sites use:
//
//   if (const char* f = ca::bundleFile()) setopt(c, CURLOPT_CAINFO, f);
//   if (const char* d = ca::bundleDir())  setopt(c, CURLOPT_CAPATH, d);

#include <functional>
#include <string>
#include <vector>

#include <unistd.h>

namespace ca
{

struct CaPaths
{
	std::string file; // -> CURLOPT_CAINFO
	std::string dir;  // -> CURLOPT_CAPATH
};

// Env vars checked (in order) for an explicit CA *file*.  SLSSTEAM_CA_BUNDLE
// is our own escape hatch and wins over the standard libcurl/OpenSSL ones.
inline const std::vector<std::string>& caFileEnvVars()
{
	static const std::vector<std::string> v = {
		"SLSSTEAM_CA_BUNDLE",
		"CURL_CA_BUNDLE",
		"SSL_CERT_FILE",
	};
	return v;
}

// Well-known CA bundle files across distros, most-common first.
inline const std::vector<std::string>& caFileCandidates()
{
	static const std::vector<std::string> v = {
		"/etc/ssl/certs/ca-certificates.crt",            // Debian/Ubuntu/Arch/SteamOS
		"/etc/pki/tls/certs/ca-bundle.crt",              // Fedora/RHEL/CentOS
		"/etc/ssl/ca-bundle.pem",                        // openSUSE
		"/etc/pki/tls/cacert.pem",                       // older RHEL
		"/etc/ssl/cert.pem",                             // Alpine/BSD (SteamOS alt symlink)
		"/etc/ca-certificates/extracted/tls-ca-bundle.pem", // Arch/SteamOS real bundle
	};
	return v;
}

// Well-known hashed-cert directories (CURLOPT_CAPATH).
inline const std::vector<std::string>& caDirCandidates()
{
	static const std::vector<std::string> v = {
		"/etc/ssl/certs",
		"/etc/pki/tls/certs",
		"/etc/ca-certificates/extracted",
	};
	return v;
}

// Pure discovery.  getenv_fn returns nullptr for an unset var; exists_fn
// reports whether a path is present.  Env file vars take precedence over the
// probe list; a set-but-missing env path is skipped (falls through).  CAPATH
// is resolved independently (SSL_CERT_DIR, then the dir probe list).
inline CaPaths discoverWith(const std::function<const char*(const char*)>& getenv_fn,
                            const std::function<bool(const std::string&)>& exists_fn)
{
	CaPaths out;

	auto envValue = [&](const char* key) -> const char* {
		const char* v = getenv_fn(key);
		return (v && v[0] != '\0') ? v : nullptr; // empty == unset
	};

	// --- CA file: env overrides, then probe list ---
	for (const std::string& key : caFileEnvVars())
	{
		if (const char* v = envValue(key.c_str()); v && exists_fn(v))
		{
			out.file = v;
			break;
		}
	}
	if (out.file.empty())
	{
		for (const std::string& cand : caFileCandidates())
		{
			if (exists_fn(cand)) { out.file = cand; break; }
		}
	}

	// --- CA dir (CAPATH): env override, then probe list ---
	if (const char* v = envValue("SSL_CERT_DIR"); v && exists_fn(v))
	{
		out.dir = v;
	}
	if (out.dir.empty())
	{
		for (const std::string& cand : caDirCandidates())
		{
			if (exists_fn(cand)) { out.dir = cand; break; }
		}
	}

	return out;
}

// Real-filesystem path-exists.  Uses access(F_OK) rather than stat: this
// library is built 32-bit WITHOUT large-file support, so a stat() on a
// modern filesystem (btrfs on SteamOS gives 64-bit inode numbers) fails with
// EOVERFLOW when the inode/size doesn't fit the legacy 32-bit struct stat —
// which would make every candidate look absent.  access() fills no struct,
// follows symlinks, and just answers the existence question.
inline bool pathExists(const std::string& p)
{
	return ::access(p.c_str(), F_OK) == 0;
}

// Cached discovery against the live environment + filesystem.
inline const CaPaths& discovered()
{
	static const CaPaths paths = discoverWith(
		[](const char* k) -> const char* { return std::getenv(k); },
		pathExists);
	return paths;
}

// nullptr when nothing was found, so the caller leaves curl on its
// compiled-in default rather than pinning a bogus path.
inline const char* bundleFile()
{
	const auto& p = discovered();
	return p.file.empty() ? nullptr : p.file.c_str();
}

inline const char* bundleDir()
{
	const auto& p = discovered();
	return p.dir.empty() ? nullptr : p.dir.c_str();
}

} // namespace ca
