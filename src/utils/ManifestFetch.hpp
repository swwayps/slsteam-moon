
#pragma once

#include <cstdint>
#include <optional>
#include <vector>


namespace ManifestFetch
{
	// One CDN host fetch outcome, reduced to the two facts the expired-code
	// retry policy cares about.
	struct CdnOutcome
	{
		bool networkError; // curl itself failed (status is meaningless)
		long httpStatus;   // HTTP response code when networkError == false
	};

	// True iff at least one host was tried and EVERY attempt failed
	// specifically with HTTP 401 (Unauthorized) and none failed for a
	// different reason.  A unanimous 401 across all CDN hosts is the
	// signature of an expired manifest request-code (codes carry a ~5-min
	// CDN TTL) and is recoverable by re-resolving a fresh code.  Any network
	// error or non-401 status (e.g. a 503 overloaded edge) is a real or
	// transient failure that re-resolving the code would not fix, so we must
	// NOT burn a provider round-trip on it.
	inline bool isExpiredCodeSignature(const std::vector<CdnOutcome>& outcomes)
	{
		if (outcomes.empty()) return false;
		for (const auto& o : outcomes)
		{
			if (o.networkError || o.httpStatus != 401) return false;
		}
		return true;
	}

	int getTimeoutSec();
	const char* defaultTimeoutKey();

	void submit(uint64_t jobId, uint64_t manifestGid,
	            uint32_t appId, uint32_t depotId);

	void submitManifestBlob(uint64_t manifestGid,
	                        uint32_t appId, uint32_t depotId);

	// Submit (or join an in-flight one) and block up to timeoutSec for the
	// manifest blob to land on disk.  Returns true if the .manifest file is
	// now present (or was already there).  Used by BYldRequestDepotManifest
	// to avoid the "no internet / hit retry" UX: Steam's first install
	// attempt finds the file already cached.
	bool awaitManifestBlob(uint64_t manifestGid, uint32_t depotId,
	                       int timeoutSec);

	bool fetchManifestBlobSync(uint64_t manifestGid, uint32_t depotId);

	std::optional<uint64_t> resolve(uint64_t jobId);

	void discard(uint64_t jobId);
}
