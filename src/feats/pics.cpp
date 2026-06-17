
#include "pics.hpp"

#include "depotkey.hpp"
#include "manifeststore.hpp"
#include "prewarm.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../update.hpp"

#include "../utils/ManifestFetch.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace PICS
{

namespace
{

std::string getCacheDir()
{
	std::stringstream ss;
	ss << g_config.getDir() << "/cache";
	const auto dir = ss.str();
	if (!std::filesystem::exists(dir))
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
	}
	return dir;
}

std::string getBufferPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".bin";
	return ss.str();
}

std::string getMetaPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getCacheDir() << "/picsbuffer_" << appId << ".yaml";
	return ss.str();
}

bool persistAppBuffer(uint32_t appId, uint32_t changeNumber,
                      const std::string& sha, const std::string& buffer)
{
	if (buffer.empty()) return false;
	if (sha.size() != 20)
	{
		g_pLog->debug("PICS: refusing to persist app=%u (bad sha size %zu)\n",
		              appId, sha.size());
		return false;
	}

	const auto bufPath = getBufferPath(appId);
	const auto metaPath = getMetaPath(appId);

	if (std::filesystem::exists(metaPath) && std::filesystem::exists(bufPath))
	{
		try
		{
			auto node = YAML::LoadFile(metaPath);
			const auto cachedChange = node["change_number"].as<uint32_t>();
			const auto cachedSize = node["wire_size"].as<size_t>();
			if (cachedChange == changeNumber && cachedSize == buffer.size())
			{
				return true;
			}
		}
		catch (...) { /* fall through to rewrite */ }
	}

	{
		std::ofstream ofs(bufPath, std::ios::binary | std::ios::trunc);
		if (!ofs.is_open())
		{
			g_pLog->debug("PICS: cannot write %s\n", bufPath.c_str());
			return false;
		}
		ofs.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
	}

	{
		YAML::Emitter em;
		em << YAML::BeginMap;
		em << YAML::Key << "appid"          << YAML::Value << appId;
		em << YAML::Key << "change_number"  << YAML::Value << changeNumber;
		em << YAML::Key << "wire_size"      << YAML::Value << buffer.size();
		em << YAML::Key << "sha_b64"        << YAML::Value << base64::to_base64(sha);
		em << YAML::EndMap;

		std::ofstream ofs(metaPath, std::ios::trunc);
		if (!ofs.is_open())
		{
			g_pLog->debug("PICS: cannot write %s\n", metaPath.c_str());
			return false;
		}
		ofs.write(em.c_str(), em.size());
	}

	g_pLog->debug("PICS: cached app=%u change=%u buffer=%zu bytes -> %s\n",
	              appId, changeNumber, buffer.size(), bufPath.c_str());
	return true;
}

std::vector<std::pair<uint32_t, uint64_t>> extractDepotsAndGids(const std::string& buf)
{
	// Single definition shared with the background pre-warm worker
	// (feats/prewarm.hpp) so the install path and the warm path mine the
	// provisioned buffer identically.
	return Prewarm::extractDepotsAndGids(buf);
}

// Read a previously-persisted product-info buffer from our cache.
// Used to recover the depot/gid list for AdditionalApps whose live CM
// product-info response carries an empty buffer (see the staging
// fallback in recvProductInfoResponse).  AppInfoProvision and
// persistAppBuffer both write to this same `picsbuffer_<appid>.bin`
// path, so whichever ran last is what we read.
std::string readCachedBuffer(uint32_t appId)
{
	const auto path = getBufferPath(appId);
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return {};
	const std::streamsize sz = ifs.tellg();
	if (sz <= 0 || sz > (8LL << 20)) return {};
	std::string out;
	out.resize(static_cast<std::size_t>(sz));
	ifs.seekg(0);
	if (!ifs.read(out.data(), sz)) return {};
	return out;
}

void cleanShaderHitCache(uint32_t appId)
{
	const char* home = std::getenv("HOME");
	if (!home) return;

	static const char* steamRoots[] = {
		"/.steam/steam",
		"/.steam/debian-installation",
		"/.local/share/Steam",
	};

	const std::string needle = std::to_string(appId) + "_pbuf";

	for (const char* suffix : steamRoots)
	{
		const auto userdataDir = std::string(home) + suffix + "/userdata";
		if (!std::filesystem::exists(userdataDir)) continue;

		std::error_code ec;
		for (auto& entry : std::filesystem::recursive_directory_iterator(userdataDir, ec))
		{
			if (!entry.is_regular_file()) continue;
			const auto fn = entry.path().filename().string();
			if (fn == needle)
			{
				g_pLog->info("PICS: removing shader hit cache: %s\n",
				             entry.path().c_str());
				std::filesystem::remove(entry.path(), ec);
			}
		}
	}
}

} // namespace

void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp)
{
	if (!resp) return;

	g_pLog->debug
	(
		"PICS: response apps=%d packages=%d unknown_apps=%d unknown_packages=%d meta_only=%i\n",
		resp->apps_size(),
		resp->packages_size(),
		resp->unknown_appids_size(),
		resp->unknown_packageids_size(),
		resp->meta_data_only() ? 1 : 0
	);


	// AdditionalApps whose live product-info buffer is empty: we must
	// stage their depot manifests ourselves, SYNCHRONOUSLY before this
	// handler returns (so they're on disk before Steam plans the install).
	// We collect them here and stage them CONCURRENTLY after the per-app
	// loop instead of blocking on each in turn — see the staging pass below.
	std::vector<AppDepots> toStage;

	const auto added = g_config.addedAppIds.get();
	for (int i = 0; i < resp->apps_size(); ++i)
	{
		auto* app = resp->mutable_apps(i);
		g_pLog->debug
		(
			"PICS: app=%u change=%u missing_token=%i only_public=%i sha_size=%zu buffer_size=%zu\n",
			app->appid(),
			app->change_number(),
			app->missing_token() ? 1 : 0,
			app->only_public() ? 1 : 0,
			app->sha().size(),
			app->buffer().size()
		);

		if (!added.count(app->appid()))
		{
			continue;
		}

		if (app->buffer().size() > 0)
		{
			// IMPORTANT: do NOT rewrite app->buffer() here.
			//
			// We used to pin manifest GIDs by editing the product-info
			// text buffer (ManifestId::applyToWireBuffer) and then
			// re-stamping app->sha().  Steam, however, validates the
			// product-info buffer against the SHA-1 it received in the
			// PICS *changelist* (the authoritative hash from the prior
			// request stage), not against the sha field in this
			// response.  Any edit to the buffer therefore fails Steam's
			// integrity check:
			//
			//     appinfo_log: "Corrupt data in text buffer for app N"
			//     "UpdatesJob: apps still needs updates, run again"
			//
			// which makes Steam re-request product info forever and
			// hangs the client at "Loading user data" on a cold cache
			// (reproduced; the loop only ever hit the AddedApps whose
			// depots had manifest pins, i.e. the ones we rewrote).
			//
			// Manifest-GID pinning is handled at the download layer
			// instead — ManifestCode's GetManifestRequestCode /
			// BYldRequestDepotManifest hooks redirect the actual
			// manifest request to the pinned gid — so dropping the
			// product-info rewrite loses nothing.  We persist the
			// pristine, server-validated buffer (verbatim sha) so the
			// appinfo.vdf warm-cache splice stays consistent too.
			persistAppBuffer(app->appid(), app->change_number(),
			                 app->sha(), app->buffer());

			cleanShaderHitCache(app->appid());
		}

		// Decide which buffer to mine for depots/gids.  For an
		// AdditionalApp whose product info isn't in the local library,
		// the live CM response carries an EMPTY buffer (no depots), so we
		// fall back to the product-info buffer we provisioned to disk
		// during setup() (picsbuffer_<appid>.bin).
		const bool emptyLiveBuffer = app->buffer().size() == 0;
		const std::string warmBuf =
		    emptyLiveBuffer ? readCachedBuffer(app->appid()) : app->buffer();
		if (warmBuf.empty())
		{
			g_pLog->debug("PICS: app=%u no buffer to stage (live=%zu, no cache)\n",
			              app->appid(), app->buffer().size());
			continue;
		}

		auto depots = extractDepotsAndGids(warmBuf);

		// Archive every depot manifest the zip shipped (all platforms)
		// into the purge-proof ManifestStore while they're still in
		// depotcache — before Steam's post-commit purge removes the
		// non-mounted ones (e.g. the windows depot of a native-linux
		// title).  This is what lets a later offline Proton-switch restore
		// + install the windows depot.  AdditionalApps only.
		if (emptyLiveBuffer)
		{
			std::vector<uint32_t> depotIds;
			depotIds.reserve(depots.size());
			for (const auto& [depotId, gid] : depots) depotIds.push_back(depotId);
			ManifestStore::archiveDepots(depotIds);
		}

		if (!emptyLiveBuffer)
		{
			// Library app: Steam drives its own manifest fetch.  A
			// best-effort async prefetch is harmless but never on the
			// critical path, so don't block the recv thread.
			for (const auto& [depotId, gid] : depots)
			{
				// Only prefetch depots we can actually decrypt.
				if (DepotKey::getCachedKey(depotId).key.empty())
				{
					continue;
				}
				g_pLog->info("PICS: prefetching manifest for app=%u depot=%u gid=%llu\n",
				             app->appid(), depotId, static_cast<unsigned long long>(gid));
				ManifestFetch::submitManifestBlob(gid, app->appid(), depotId);
			}
			continue;
		}

		// AdditionalApp: defer staging to the concurrent pass below.  The
		// key filter (we only stage depots we hold a key for — staging a
		// blob we can't decrypt just wastes a CDN round-trip) is applied
		// there, in buildSyncStagePlan.
		toStage.push_back({app->appid(), std::move(depots)});
	}

	// Stage every AdditionalApp depot manifest CONCURRENTLY, before this
	// handler returns.
	//
	// Why staging must finish before we return (verified on the VM
	// 2026-06-04, superseding the HANDOFF "timing race" theory):
	// clicking Install triggers a fresh PICS product-info request, and
	// Steam cannot begin update *planning* until this response is
	// processed (it's what tells Steam which depots/manifests exist), so
	// this recv handler strictly precedes planning.  During planning Steam
	// decides whether to call CDepotDownloadMgr::BYldRequestDepotManifest:
	//   - manifest NOT on disk at planning -> Steam calls BYld -> the
	//     ORIGINAL BYld returns 'Access Denied' -> the attempt is canceled
	//     with "No connection" (only the ~30s auto-retry, which left the
	//     blob on disk, ever recovered);
	//   - manifest ALREADY on disk at planning -> Steam SKIPS BYld and goes
	//     straight to Downloading -> success.
	// So the blobs must be on disk before we return.  This runs on a
	// genuine Steam worker thread (the InitFromPacket detour).
	//
	// We used to await each depot SEQUENTIALLY (awaitManifestBlob in the
	// loop above), which on a cold cache cost ~0.4-0.5s per depot and, on a
	// big title (DL2, 36 depots), blocked this thread — the one that also
	// answers the Install dialog's button IPCs — for 15s+, freezing every
	// button.  We now keep the guarantee but kick off ALL fetches first,
	// then await them, so the wall time is the slowest fetch (~1-2s) rather
	// than the sum.  ManifestFetch dedups by (gid, depotId) and the await
	// just joins the in-flight fetch, so this stages exactly the same set,
	// only concurrently.
	const auto plan = buildSyncStagePlan(
	    toStage,
	    [](uint32_t depotId)
	    {
	        return !DepotKey::getCachedKey(depotId).key.empty();
	    });

	// Pass 1: kick off every fetch (async, deduped at the fetch layer).
	for (const auto& t : plan)
	{
		g_pLog->info("PICS: staging manifest for app=%u depot=%u gid=%llu (concurrent)\n",
		             t.appId, t.depotId, static_cast<unsigned long long>(t.gid));
		ManifestFetch::submitManifestBlob(t.gid, t.appId, t.depotId);
	}

	// Pass 2: block until each lands on disk (joins the in-flight fetch).
	for (const auto& t : plan)
	{
		const bool staged = ManifestFetch::awaitManifestBlob(
		    t.gid, t.depotId, ManifestFetch::getTimeoutSec());
		g_pLog->info("PICS: manifest staging for app=%u depot=%u gid=%llu -> %s\n",
		             t.appId, t.depotId, static_cast<unsigned long long>(t.gid),
		             staged ? "on disk" : "FAILED (will fall back to BYld retry)");
	}

	if (resp->unknown_appids_size() > 0)
	{
		std::stringstream ss;
		for (int i = 0; i < resp->unknown_appids_size(); ++i)
		{
			if (i) ss << ',';
			ss << resp->unknown_appids(i);
		}
		g_pLog->debug("PICS: unknown_appids=[%s]\n", ss.str().c_str());
	}

	// Start the background manifest pre-warm worker now that we're on a
	// real Steam worker thread (post-login PICS recv).  ensureStarted() is
	// idempotent, so calling it on every recv is cheap.  It keeps every
	// AddedApp's depot manifests (all OSes we hold a key for, incl. DLC)
	// staged on disk, healing the post-commit purge so a later planning
	// pass — e.g. the user forcing a Proton compat tool, which re-plans to
	// the windows depots without a fresh PICS request — finds the manifests
	// already present and skips BYldRequestDepotManifest (no ~30s retry).
	// MUST NOT be started from load()/setup() (HANDOFF DEAD END #2).
	Prewarm::ensureStarted();

	// Refresh the safe-mode-hash cache (updates.yaml) off the boot path.
	// init() served it from disk synchronously so Steam's launch never
	// blocks on GitHub; this brings it up to date from a real worker
	// thread, gated by a TTL so we don't fetch on every relaunch.
	Updater::refreshInBackgroundIfStale();
}

void recvMsg(CProtoBufMsgBase* msg)
{
	if (!msg) return;
	switch (msg->type)
	{
		case EMSG_PICS_PRODUCTINFO_RESPONSE:
			recvProductInfoResponse(msg->getBody<CMsgClientPICSProductInfoResponse>());
			break;
		default:
			break;
	}
}

} // namespace PICS
