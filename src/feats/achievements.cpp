#include "achievements.hpp"

#include "../config.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/EResult.hpp"

#include "../log.hpp"

#include <chrono>
#include <mutex>


namespace
{
	// One process-wide gate shared by the send/recv handlers. Both run on
	// Steam worker threads (the InitFromPacket / Send detours), so guard it.
	Achievements::SpoofTracker g_spoofTracker(30);
	std::mutex g_spoofMutex;

	uint64_t nowSeconds()
	{
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count());
	}
}


// Outgoing CMsgClientGetUserStats (eMsg 818): for an AdditionalApp,
// impersonate a real owner of the game so Steam's servers return a
// populated achievement schema instead of refusing (eresult=Fail). The
// account doesn't genuinely own the title, so a plain request comes back
// empty; querying as an owner is what makes the schema available.
void Achievements::sendMessage(CProtoBufMsgBase* msg)
{
	if (!g_config.achievements.get())
	{
		return;
	}

	if (msg->type != EMSG_REQUEST_USERSTATS)
	{
		return;
	}

	auto body = msg->getBody<CMsgClientGetUserStats>();
	if (!body->has_game_id())
	{
		return;
	}

	const uint32_t appId = static_cast<uint32_t>(body->game_id());
	if (!g_config.isAddedAppId(appId))
	{
		return;
	}

	const uint64_t owner = resolveOwnerSteamId(
		appId,
		g_config.achievementOwners.get(),
		g_config.achievementOwnerId.get());

	// Force a fresh fetch every time: drop the local validation token and
	// version so the server can't short-circuit with eresult=Fail on a
	// stale crc, then impersonate the owner.
	body->clear_crc_stats();
	body->set_schema_local_version(-1);
	body->set_steam_id_for_user(owner);

	{
		std::lock_guard<std::mutex> guard(g_spoofMutex);
		g_spoofTracker.mark(appId, nowSeconds());
	}

	g_pLog->debug("Achievements: requesting stats schema for %u as owner\n", appId);
}


// Incoming CMsgClientGetUserStatsResponse (eMsg 819): if this is the reply
// to a request we just spoofed, the stats inside belong to the impersonated
// owner. Strip them so the user starts clean and tracks unlocks locally,
// clear the crc so Steam re-fetches the schema next launch instead of
// trusting an empty cache, and force eresult=OK so the UI accepts the reply.
//
// Guards:
//   - Only AdditionalApps are touched (genuinely-owned games pass through).
//   - Only responses to a request WE spoofed are touched; a pass-through
//     reply (Steam already had a cached schema) is left alone so we don't
//     overwrite a good cache with "0 unlocks".
//   - Only responses that actually carry a schema are rewritten; a failed
//     or empty reply (the impersonated owner doesn't own this title) is
//     left untouched, because forcing eresult=OK on an empty body would
//     tell Steam the game has zero achievements and blank the panel.
void Achievements::recvMessage(const CProtoBufMsgBase* msg)
{
	if (!g_config.achievements.get())
	{
		return;
	}

	if (msg->type != EMSG_REQUEST_USERSTATS_RESPONSE)
	{
		return;
	}

	auto body = msg->getBody<CMsgClientGetUserStatsResponse>();
	if (!body->has_game_id())
	{
		return;
	}

	const uint32_t appId = static_cast<uint32_t>(body->game_id());
	if (!g_config.isAddedAppId(appId))
	{
		return;
	}

	bool wasSpoofed;
	{
		std::lock_guard<std::mutex> guard(g_spoofMutex);
		wasSpoofed = g_spoofTracker.consume(appId, nowSeconds());
	}
	if (!wasSpoofed)
	{
		return;
	}

	const bool gotSchema = body->has_schema() && !body->schema().empty();
	if (!gotSchema)
	{
		g_pLog->debug(
			"Achievements: no schema returned for %u (configured owner may not own it)\n",
			appId);
		return;
	}

	body->clear_stats();
	body->clear_achievement_blocks();
	body->clear_crc_stats();
	body->set_eresult(ERESULT_OK);

	g_pLog->debug("Achievements: serving fetched stats schema for %u\n", appId);
}
