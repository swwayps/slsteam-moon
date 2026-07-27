#include "ticket.hpp"

#include "fakeappid.hpp"
#include "apps.hpp"

#include "../config.hpp"
#include "../globals.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/EResult.hpp"
#include "../sdk/IClientUtils.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/emittermanip.h"

#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <sstream>

uint32_t Ticket::oneTimeSteamIdSpoof = 0;
std::map<uint32_t, Ticket::SavedTicket> Ticket::ticketMap = std::map<uint32_t, SavedTicket>();
std::map<uint32_t, Ticket::SavedTicket> Ticket::encryptedTicketMap = std::map<uint32_t, SavedTicket>();

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

	auto node = YAML::LoadFile(path);
	ticket.steamId = node["steamId"].as<uint32_t>();
	ticket.ticket = std::string
	(
		base64::from_base64(node["ticket"].as<std::string>())
	);
	//g_pLog->debug("Ticket: %u, %s\n", ticket.steamId, ticket.ticket.c_str());

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
	node << YAML::Value << g_currentSteamId.steamId;
	node << YAML::Key << "ticket";
	node << YAML::Value << base64::to_base64(bytes);
	node << YAML::EndMap;

	const auto path = Ticket::getTicketPath(appId);
	std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
	if (!ofs.is_open()) return false;
	ofs.write(node.c_str(), node.size());
	if (!ofs.good()) return false;

	g_pLog->infoOnce("Saved ticket for %u\n", appId);

	//TODO: Skip copy
	SavedTicket ticket {};
	ticket.ticket = bytes;
	ticketMap[appId] = ticket;
	return true;
}

void Ticket::launchApp(uint32_t appId)
{
	auto ticket = getCachedTicket(appId);
	if (!ticket.ticket.size())
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

void Ticket::getTicketOwnershipExtendedData(uint32_t appId)
{
	const SavedTicket cached = Ticket::getCachedTicket(appId);
	const uint32_t steamId = cached.steamId;
	if (!steamId)
	{
		return;
	}

	oneTimeSteamIdSpoof = steamId;
}

std::string Ticket::getEncryptedTicketPath(uint32_t appId)
{
	std::stringstream ss;
	ss << getTicketDir().c_str() << "/encryptedTicket_" << appId << ".yaml";

	return ss.str();
}

Ticket::SavedTicket Ticket::getCachedEncryptedTicket(uint32_t appId)
{
	const uint32_t realAppId = FakeAppIds::getRealAppIdForCurrentPipe();
	const uint32_t fakeAppId = FakeAppIds::getFakeAppId(realAppId);

	SavedTicket ticket {};

	if (realAppId && fakeAppId && appId != realAppId)
	{
		g_pLog->infoOnce("Returning empty cached encrypted ticket for %u because it's set to %u\n", realAppId, fakeAppId);
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

	auto node = YAML::LoadFile(path);
	ticket.steamId = node["steamId"].as<uint32_t>();
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
	//g_pLog->debug("Ticket: %u, %s\n", ticket.steamId, ticket.ticket.c_str());

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
	node << YAML::Value << g_currentSteamId.steamId;
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

	//TODO: Skip copy
	SavedTicket ticket {};
	ticket.steamId = g_currentSteamId.steamId;
	ticket.ticket = bytes;
	encryptedTicketMap[appId] = ticket;
	return true;
}

void Ticket::recvEncryptedAppTicket(CMsgClientRequestEncryptedAppTicketResponse* msg)
{
	if (msg->eresult() == ERESULT_OK)
	{
		saveEncryptedTicketToCache(msg);
		return;
	}

	SavedTicket ticket = getCachedEncryptedTicket(msg->app_id());
	if(!ticket.steamId)
	{
		return;
	}

	msg->ParseFromString(ticket.ticket);
	g_pLog->debug("Using encryptedTicket_%u from disk\n", msg->app_id());
}

void Ticket::recvAppTicket(CMsgClientGetAppOwnershipTicketResponse* msg)
{
	if(msg->eresult() == ERESULT_OK)
	{
		saveTicketToCache(msg);
		return;
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
		return;
	}

	//We do not load tickets from disk in the network layer, otherwise they won't be loaded in offline mode
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
