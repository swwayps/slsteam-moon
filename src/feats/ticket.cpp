#include "ticket.hpp"

#include "fakeappid.hpp"
#include "apps.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../process.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/EResult.hpp"
#include "../sdk/IClientUtils.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/emittermanip.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <sstream>

std::unordered_map<AppId_t, CSteamId> Ticket::oneTimeSteamIdSpoof = std::unordered_map<AppId_t, CSteamId>();
std::unordered_map<AppId_t, Ticket::SavedTicket> Ticket::ticketMap = std::unordered_map<AppId_t, SavedTicket>();
std::unordered_map<AppId_t, Ticket::SavedTicket> Ticket::encryptedTicketMap = std::unordered_map<AppId_t, SavedTicket>();

std::string Ticket::getTicketDir()
{
	std::stringstream ss;
	ss << g_config.getDir().c_str() << "/cache";

	const auto dir = ss.str();
	if (!std::filesystem::exists(dir.c_str()))
	{
		std::filesystem::create_directory(dir.c_str());
	}

	return ss.str();
}

std::string Ticket::getTicketPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getTicketDir().c_str() << "/ticket_" << appId << ".yaml";

	return ss.str();
}

Ticket::SavedTicket Ticket::getCachedTicket(uint32_t appId)
{
	std::lock_guard<std::mutex> lock(cacheMutex);
	if (invalidatedApps.contains(appId)) return {};

	const auto it = ticketMap.find(appId);
	if (it != ticketMap.end()) return it->second;

	SavedTicket ticket {};
	const auto path = getTicketPath(appId);
	if (!std::filesystem::exists(path.c_str()))
	{
		return ticket;
	}

	g_pLog->debug("Reading ticket for %u\n", appId);

	// The cache file is untrusted on-disk state: a truncated or corrupt
	// ticket_<appid>.yaml makes YAML::LoadFile / node.as<>() / base64 decode
	// throw. Uncaught on this IPC path the throw aborts the whole client. The
	// release build pins -fno-reorder-blocks-and-partition, keeping this catch
	// a reliable backstop; treat any parse failure as a cache miss.
	try
	{
		auto node = YAML::LoadFile(path);
		ticket.steamId = CSteamId(node["steamId"].as<uint64_t>());
		ticket.ticket = std::string
		(
			base64::from_base64(node["ticket"].as<std::string>())
		);
	}
	catch (const std::exception& e)
	{
		g_pLog->warn("Ignoring corrupt ticket cache for %u: %s\n", appId, e.what());
		return {};
	}

	// Keep the disk read and map publication in one transaction with the
	// invalidation check. forgetApp() cannot interleave and leave a late cache
	// entry for an app that was removed while this load was in progress.
	ticketMap[appId] = ticket;
	return ticket;
}

bool Ticket::saveTicketToCache(CMsgClientGetAppOwnershipTicketResponse* resp)
{
	const uint32_t appId = resp->app_id();

	g_pLog->debug("Saving ticket for %u...\n", appId);

	auto bytes = resp->ticket();

	std::lock_guard<std::mutex> lock(cacheMutex);
	if (invalidatedApps.contains(appId)) return false;

	YAML::Emitter node;
	node << YAML::BeginMap;
	node << YAML::Key << "steamId";
	node << YAML::Value << g_currentSteamId.steamId64;
	node << YAML::Key << "ticket";
	node << YAML::Value << base64::to_base64(bytes);
	node << YAML::EndMap;

	const auto path = Ticket::getTicketPath(appId);
	std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
	if (!ofs.is_open()) return false;
	ofs.write(node.c_str(), node.size());
	if (!ofs.good()) return false;

	g_pLog->infoOnce("Saved ticket for %u\n", appId);

	SavedTicket& ticket = ticketMap[appId];
	ticket.steamId = g_currentSteamId;
	ticket.ticket = bytes;
	return true;
}

void Ticket::connectPipe(const HSteamPipe pipe)
{
	const auto process = g_processMap.find(pipe);
	if (process == g_processMap.end())
	{
		return;
	}
	const auto& proc = process->second;

	if (!proc.steamDRM)
	{
		return;
	}

	const SavedTicket ticket = getCachedTicket(proc.appId);
	if (!ticket.isValid())
	{
		return;
	}

	oneTimeSteamIdSpoof[proc.appId] = ticket.steamId;
}

void Ticket::launchApp(uint32_t appId)
{
	auto ticket = getCachedTicket(appId);
	if (!ticket.isValid())
	{
		return;
	}

	CUser* user = getLocalUser();
	if (user == nullptr)
	{
		g_pLog->debug("Ticket::launchApp(%u): no local user yet; skipping\n", appId);
		return;
	}
	user->updateAppOwnershipTicket(appId, reinterpret_cast<void*>(ticket.ticket.data()), ticket.ticket.size());
	g_pLog->infoOnce("Force loaded AppOwnershipTicket for %i\n", appId);
}

void Ticket::getEncryptedAppTicket(uint32_t appId)
{
	const SavedTicket cached = Ticket::getCachedEncryptedTicket(appId);
	if (!cached.isValid())
	{
		return;
	}

	oneTimeSteamIdSpoof[appId] = cached.steamId;
}

void Ticket::getTicketOwnershipExtendedData(uint32_t appId)
{
	if ((g_config.smartTickets.get() & CConfig::k_ESmartTicketsSteamDRM)
		&& g_pSteamEngine)
	{
		const auto utils = g_pSteamEngine->getUtils();
		const auto process = utils
			? g_processMap.find(utils->getCurrentSteamPipe())
			: g_processMap.end();
		if (process != g_processMap.end() && process->second.steamDRM)
		{
			//Handled in connectPipe
			//For other ticket requests we fall through to spoofing the next GetSteamID call
			return;
		}
	}

	const SavedTicket cached = Ticket::getCachedTicket(appId);
	if (!cached.isValid())
	{
		return;
	}

	oneTimeSteamIdSpoof[appId] = cached.steamId;
}

std::string Ticket::getEncryptedTicketPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getTicketDir().c_str() << "/encryptedTicket_" << appId << ".yaml";

	return ss.str();
}

Ticket::SavedTicket Ticket::getCachedEncryptedTicket(uint32_t appId)
{
	SavedTicket ticket {};
	const AppId_t fakeAppId = FakeAppIds::getFakeAppId(appId);
	const auto smartTickets = g_config.smartTickets.get();

	if (!(smartTickets & CConfig::k_ESmartTicketsDenuvo) && appId && fakeAppId && fakeAppId != appId)
	{
		g_pLog->infoOnce("Returning empty cached encrypted Ticket for %u because it's running as %u\n", appId, fakeAppId);
		return ticket;
	}

	std::lock_guard<std::mutex> lock(cacheMutex);
	if (invalidatedApps.contains(appId)) return {};

	const auto it = encryptedTicketMap.find(appId);
	if (it != encryptedTicketMap.end()) return it->second;

	const auto path = getEncryptedTicketPath(appId);
	if (!std::filesystem::exists(path.c_str()))
	{
		return ticket;
	}

	g_pLog->debug("Reading encrypted ticket for %u\n", appId);

	// Same untrusted-cache guard as getCachedTicket: a corrupt
	// encryptedTicket_<appid>.yaml must degrade to a cache miss, not abort the
	// client from this IPC path.
	try
	{
		auto node = YAML::LoadFile(path);
		ticket.steamId = CSteamId(node["steamId"].as<uint64_t>());
		ticket.ticket = std::string
		(
			//Can not get yaml-cpp to properly decode
			//TODO: Investigate
			//reinterpret_cast<const char*>
			//(
			//	&YAML::DecodeBase64(node["encryptedTicket"].as<std::string>()).at(0)
			//)
			base64::from_base64(node["encryptedTicket"].as<std::string>())
		);
	}
	catch (const std::exception& e)
	{
		g_pLog->warn("Ignoring corrupt encrypted ticket cache for %u: %s\n", appId, e.what());
		return {};
	}

	// Keep the disk read and map publication in one transaction with the
	// invalidation check, just like ordinary ownership tickets.
	encryptedTicketMap[appId] = ticket;
	return ticket;
}

bool Ticket::saveEncryptedTicketToCache(CMsgClientRequestEncryptedAppTicketResponse* resp)
{
	const uint32_t appId = resp->app_id();

	g_pLog->debug("Saving encrypted ticket for %u...\n", appId);

	auto bytes = resp->SerializeAsString();

	std::lock_guard<std::mutex> lock(cacheMutex);
	if (invalidatedApps.contains(appId)) return false;

	YAML::Emitter node;
	node << YAML::BeginMap;
	node << YAML::Key << "steamId";
	node << YAML::Value << g_currentSteamId.steamId64;
	node << YAML::Key << "encryptedTicket";
	//node << YAML::Value << YAML::EncodeBase64(reinterpret_cast<const unsigned char*>(bytes.c_str()), bytes.size());
	node << YAML::Value << base64::to_base64(bytes);
	node << YAML::EndMap;

	const auto path = getEncryptedTicketPath(appId);
	std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
	if (!ofs.is_open()) return false;
	ofs.write(node.c_str(), node.size());
	if (!ofs.good()) return false;

	g_pLog->infoOnce("Saved encrypted ticket for %u\n", appId);

	SavedTicket& ticket = encryptedTicketMap[appId];
	ticket.steamId = g_currentSteamId;
	ticket.ticket = bytes;
	return true;
}

bool Ticket::recvEncryptedAppTicket(CMsgClientRequestEncryptedAppTicketResponse* msg)
{
	if (msg->eresult() == ERESULT_OK)
	{
		saveEncryptedTicketToCache(msg);
		return false;
	}

	SavedTicket ticket = getCachedEncryptedTicket(msg->app_id());
	if(!ticket.isValid())
	{
		return false;
	}

	msg->ParseFromString(ticket.ticket);
	g_pLog->debug("Using encryptedTicket_%u from disk\n", msg->app_id());
	return true;
}

bool Ticket::recvAppTicket(CMsgClientGetAppOwnershipTicketResponse* msg)
{
	if(msg->eresult() == ERESULT_OK)
	{
		saveTicketToCache(msg);
		return false;
	}

	const uint32_t appId = msg->app_id();

	// For AdditionalApps the CM legitimately returns a non-OK eresult on
	// the ownership ticket request, and Steam's downloader treats that
	// as a hard fault: it won't even progress to GetManifestRequestCode,
	// so the install stalls at "Failed downloading 1 manifests
	// (Connection timeout)" minutes later.
	//
	// Stamp eresult=OK on the parsed message so the downloader proceeds.
	// We intentionally do NOT touch the `ticket` string field — that
	// lives in Steam's protobuf arena and rewriting it has corrupted the
	// heap in past experiments.  set_eresult is
	// a trivial int32 mutation, no allocation.  If Steam's later pipeline
	// strictly validates the ticket bytes we may need to re-route this
	// through a fresh message buffer (mirror what hkBRouteMsgToJob does
	// for GetManifestRequestCode), but try the minimal change first.
	if (Ticket::shouldStampAppOwnershipTicket(
			g_config.isAddedAppId(appId), Apps::isAddedAppDlcId(appId)))
	{
		msg->set_eresult(static_cast<int32_t>(ERESULT_OK));
		// One-time log so we don't spam every retry.  Note: do NOT
		// use infoOnce here — a bug in CLog dedup-thread interaction
		// has caused crashes in the past.  A plain `info` is safe;
		// CM only sends a handful of these per session.
		g_pLog->info("Ticket: stamped eresult=OK on AppOwnershipTicket response for AdditionalApp=%u\n",
		             appId);
		return true;
	}

	//We do not load tickets from disk in the network layer, otherwise they won't be loaded in offline mode
	return false;
}

void Ticket::recvMsg(CProtoBufMsgBase* msg)
{
	switch(msg->type)
	{
		case EMSG_APPOWNERSHIPTICKET_RESPONSE:
			recvAppTicket(msg->getBody<CMsgClientGetAppOwnershipTicketResponse>());
			break;

		case EMSG_ENCRYPTED_APPTICKET_RESPONSE:
			recvEncryptedAppTicket(msg->getBody<CMsgClientRequestEncryptedAppTicketResponse>());
			break;
	}
}
