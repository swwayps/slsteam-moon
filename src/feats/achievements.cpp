#include "achievements.hpp"
#include "stats_policy.hpp"
#include "playerstats.hpp"
#include "../config.hpp"
#include "../log.hpp"
#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../utils/atomic_file.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace
{
struct Pending
{
	uint32_t app, account;
	uint64_t epoch;
	bool modern;
	uint64_t owner;
	Achievements::OwnerSource source;
};
std::mutex pendingMutex;
std::unordered_map<uint64_t, Pending> pending;
Achievements::OwnerDirectory owners;
bool validJob(uint64_t job) { return job != 0 && job != UINT64_MAX; }

// Rewritten wire bytes in the shape CNetPacket::serialize expects.
struct RawBody
{
	const std::vector<uint8_t>& bytes;
	std::size_t ByteSizeLong() const { return bytes.size(); }
	bool SerializeToArray(void* out, int size) const
	{
		if (size > 0) std::memcpy(out, bytes.data(), static_cast<std::size_t>(size));
		return true;
	}
};

uint64_t nowSecs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* sourceName(Achievements::OwnerSource source)
{
	switch (source)
	{
		case Achievements::OwnerSource::Override: return "override";
		case Achievements::OwnerSource::Preferred: return "preferred";
		case Achievements::OwnerSource::Fallback: return "fallback";
		case Achievements::OwnerSource::Candidate: return "reviewer";
		case Achievements::OwnerSource::None: break;
	}
	return "none";
}

const char* verdictName(Achievements::OwnerVerdict verdict)
{
	switch (verdict)
	{
		case Achievements::OwnerVerdict::Good: return "carried the schema";
		case Achievements::OwnerVerdict::Bad: return "was refused";
		case Achievements::OwnerVerdict::Unknown: break;
	}
	return "was inconclusive";
}

// What the owner's reply says about that owner, independent of the local
// fail-closed checks: an explicit failure rules the owner out for this game.
Achievements::OwnerVerdict ownerVerdict(bool modern, const uint8_t* data,
	uint32_t size, const CMsgProtoBufHeader& header)
{
	int32_t result = 0;
	bool schema = false;
	if (modern)
	{
		if (!header.has_eresult()) return Achievements::OwnerVerdict::Unknown;
		result = header.eresult();
		schema = PlayerStats::detail::walk(data, size, [](const auto&) {}) &&
			!PlayerStats::fieldBytes(data, size, 3).empty();
	}
	else
	{
		CMsgClientGetUserStatsResponse body;
		if (!body.ParseFromArray(data, size) || !body.has_eresult())
			return Achievements::OwnerVerdict::Unknown;
		result = body.eresult();
		schema = body.has_schema() && !body.schema().empty();
	}
	if (result == k_EResultOK && schema) return Achievements::OwnerVerdict::Good;
	if (result == k_EResultFailure) return Achievements::OwnerVerdict::Bad;
	return Achievements::OwnerVerdict::Unknown;
}

std::string ownersPath()
{
	return (std::filesystem::path(g_config.getDir()) / "cache" / "achievement_owners.txt")
		.string();
}
constexpr std::uintmax_t kMaxOwnersFile = 1u << 20;
}

void Achievements::offerOwners(uint32_t app, const std::vector<uint64_t>& found)
{
	owners.offer(app, found);
	requestOwnerSave();
}

void Achievements::loadOwners()
{
	try
	{
		const auto path = ownersPath();
		std::error_code error;
		const auto size = std::filesystem::file_size(path, error);
		if (error || size > kMaxOwnersFile) return;
		std::ifstream file(path, std::ios::binary);
		owners.restore(std::string(std::istreambuf_iterator<char>(file),
		                           std::istreambuf_iterator<char>()));
	}
	catch (...)
	{
		// A damaged file only costs what was learned before.
	}
}

void Achievements::saveOwners()
{
	try
	{
		const auto path = ownersPath();
		std::error_code ignored;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
		const std::string text = owners.serialize();
		std::string error;
		if (!AtomicFile::write(path, text.data(), text.size(), error))
			g_pLog->warn("Achievements: learned owners not saved: %s\n", error.c_str());
	}
	catch (const std::exception& e)
	{
		g_pLog->warn("Achievements: learned owners not saved: %s\n", e.what());
	}
}

// At the serialized CM boundary the jobid is final, and CloudRedirect has
// already had the opportunity to serve the original SELF request. Work on
// our own protobuf objects, never Steam-owned strings/arenas.
std::vector<uint8_t> Achievements::rewriteRequest(bool modern,
	const uint8_t* data, uint32_t size, const CMsgProtoBufHeader& header)
{
	if (!g_config.achievements.get() || size > maxReplyBytes ||
	    header.ByteSizeLong() > maxHeaderBytes || !header.has_jobid_source() ||
	    !validJob(header.jobid_source())) return {};
	const uint32_t account = StatsPolicy::account();
	if (!account || (header.has_steamid() &&
	    !StatsPolicy::isSelf(header.steamid(), account))) return {};
	uint32_t app = 0;
	uint64_t target = 0;
	CMsgClientGetUserStats legacy;
	if (modern)
	{
		bool appSeen = false, targetSeen = false, valid = true;
		if (!PlayerStats::detail::walk(data, size, [&](const auto& field) {
			if (field.number != 1 && field.number != 2) return;
			bool& seen = field.number == 1 ? targetSeen : appSeen;
			if (seen || field.wireType != 0) { valid = false; return; }
			seen = true;
			std::size_t offset = field.valueOff;
			uint64_t value = 0;
			if (!PlayerStats::detail::readVarint(data, size, offset, value) ||
			    (field.number == 2 && (!value || value > UINT32_MAX))) valid = false;
		}) || !valid) return {};
		auto id = PlayerStats::parseRequestAppId(data, size);
		if (!id) return {};
		app = *id;
		target = PlayerStats::parseRequestSteamId(data, size).value_or(0);
	}
	else
	{
		if (!legacy.ParseFromArray(data, size) || !legacy.has_game_id() ||
		    legacy.game_id() > UINT32_MAX) return {};
		app = static_cast<uint32_t>(legacy.game_id());
		target = legacy.steam_id_for_user();
	}
	if (!StatsPolicy::isSelf(target, account)) return {};
	const uint64_t epoch = StatsPolicy::localEpoch(app, account, true);
	if (!epoch) return {};
	const auto overrides = g_config.achievementOwners.get();
	const auto overrideIt = overrides.find(app);
	const uint32_t lookupCount = g_config.maxSchemaTries.get();
	const auto choice = owners.choose(app,
		overrideIt != overrides.end() ? overrideIt->second : 0,
		g_config.achievementOwnerId.get(), 0x0110000100000000ULL | account,
		lookupCount != 0, nowSecs());
	if (choice.lookup)
	{
		// Steam's send thread only queues the lookup; this request goes out
		// unchanged and a later one uses what the lookup found.
		g_pLog->debug("Achievements: app=%u has no schema owner; looking up reviewers\n", app);
		requestOwnerLookup(app, lookupCount);
	}
	const uint64_t owner = choice.owner;
	if (!owner || StatsPolicy::isSelf(owner, account)) return {};
	std::vector<uint8_t> out;
	if (modern) out = PlayerStats::buildSpoofedRequest(owner, app);
	else
	{
		legacy.clear_crc_stats();
		legacy.set_schema_local_version(-1);
		legacy.set_steam_id_for_user(owner);
		out.resize(legacy.ByteSizeLong());
		if (!legacy.SerializeToArray(out.data(), out.size())) return {};
	}
	{
		std::lock_guard lock(pendingMutex);
		// Retain unanswered jobs as bounded tombstones. Expiring by time would
		// allow a late owner reply through with its achievement progress intact.
		if (auto it = pending.find(header.jobid_source()); it != pending.end())
		{
			it->second.epoch = 0; // Ambiguous duplicate: any reply must fail safely.
			return {};
		}
		if (pending.size() >= 4096)
		{
			// The table only drains on replies, so a full one stops the
			// rewrite for this session. Say so once.
			static std::atomic<bool> reported{false};
			if (!reported.exchange(true))
				g_pLog->warn("Achievements: 4096 schema requests are unanswered; "
				             "owner schema rewrites stop until Steam restarts\n");
			return {};
		}
		if (out.size() > maxReplyBytes) return {};
		pending.emplace(header.jobid_source(),
			Pending{app, account, epoch, modern, owner, choice.source});
	}
	g_pLog->debug("Achievements: app=%u schema request via %s owner\n",
	              app, sourceName(choice.source));
	return out;
}

std::optional<std::vector<uint8_t>> Achievements::rewriteResponse(bool modern,
	const uint8_t* data, uint32_t size, CMsgProtoBufHeader& header)
{
	if (!header.has_jobid_target() || !validJob(header.jobid_target())) return {};
	Pending request{};
	{
		std::lock_guard lock(pendingMutex);
		auto it = pending.find(header.jobid_target());
		if (it == pending.end() || it->second.modern != modern) return {};
		request = it->second;
		pending.erase(it);
	}
	// A tombstoned (duplicate) job cannot tell which owner answered.
	if (request.epoch && size <= maxReplyBytes)
	{
		const auto verdict = ownerVerdict(modern, data, size, header);
		if (owners.report(request.app, request.owner, request.source, verdict))
			requestOwnerSave();
		g_pLog->debug("Achievements: app=%u %s owner reply %s\n", request.app,
		              sourceName(request.source), verdictName(verdict));
	}
	const auto failure = [&]() -> std::vector<uint8_t> {
		if (header.ByteSizeLong() > maxHeaderBytes)
		{
			const auto job = header.jobid_target();
			const auto steam = header.steamid();
			const auto session = header.client_sessionid();
			header.Clear();
			header.set_jobid_target(job);
			header.set_steamid(steam);
			header.set_client_sessionid(session);
		}
		header.set_eresult(2); // A rewritten request must NEVER leak another user's progress.
		if (modern) return {};
		CMsgClientGetUserStatsResponse body;
		body.set_game_id(request.app);
		body.set_eresult(2);
		std::vector<uint8_t> out(body.ByteSizeLong());
		body.SerializeToArray(out.data(), out.size());
		return out;
	};
	if (!g_config.achievements.get() || !request.epoch || size > maxReplyBytes ||
	    header.ByteSizeLong() > maxHeaderBytes ||
	    StatsPolicy::localEpoch(request.app, request.account, true) != request.epoch ||
	    (header.has_steamid() && !StatsPolicy::isSelf(header.steamid(), request.account)))
		return failure();
	if (modern)
	{
		if (!PlayerStats::detail::walk(data, size, [](const auto&) {}) ||
		    PlayerStats::fieldBytes(data, size, 3).empty()) return failure();
		std::vector<uint8_t> out;
		PlayerStats::detail::walk(data, size, [&](const auto& field) {
			if (field.number != 2 && field.number != 4)
				out.insert(out.end(), data + field.start, data + field.end);
		});
		return out;
	}
	CMsgClientGetUserStatsResponse body;
	if (!body.ParseFromArray(data, size) || body.game_id() != request.app ||
	    !body.has_schema() || body.schema().empty()) return failure();
	body.clear_stats();
	body.clear_achievement_blocks();
	body.clear_crc_stats();
	body.set_eresult(k_EResultOK);
	std::vector<uint8_t> out(body.ByteSizeLong());
	if (!body.SerializeToArray(out.data(), out.size())) return failure();
	return out;
}

void Achievements::recvMessage(uint32_t type, bool hasSteamId, uint64_t steamId)
{
	// ClientLogOnResponse=751, ClientLoggedOff=757, ClientLicenseList=780.
	if (type != 751 && type != 757 && type != 780) return;
	StatsPolicy::invalidate();
	if (type == 757) StatsPolicy::setAccount(0);
	else if (hasSteamId)
	{
		const auto account = static_cast<uint32_t>(steamId);
		if (steamId && StatsPolicy::isSelf(steamId, account)) StatsPolicy::setAccount(account);
	}
	// Keep bounded outstanding jobs so their late replies are rejected rather
	// than allowing the previously selected owner's progress through unchanged.
}

bool Achievements::recvReply(CNetPacket* pkt)
{
	if (!pkt || !pkt->isValid()) return false;
	CMsgProtoBufHeader header;
	if (!pkt->deserializeHeader(header) || !header.has_jobid_target()) return false;
	CNetPacketBody* const before = pkt->body();
	const uint32_t bytes = pkt->sizeBytes();
	const uint32_t offset = sizeof(CNetPacketBody) + before->headerSize;
	if (offset > bytes) return false;
	const auto reply = rewriteResponse(
		pkt->getProtoBufType() == k_EMsgServiceMethodResponse,
		reinterpret_cast<const uint8_t*>(before) + offset, bytes - offset, header);
	if (!reply) return false;
	pkt->serialize(RawBody{*reply}, &header);
	if (pkt->body() != before) return true;
	g_pLog->warn("Achievements: owner reply rewrite of %zu bytes could not be applied\n",
	             reply->size());
	return false;
}
