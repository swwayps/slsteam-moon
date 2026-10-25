// A ticket save that fails is reported and leaves the saved ticket file as it
// was (both ticket kinds).
// Build: make test-ticket-cache (links the whole object set).
#include "../src/feats/ticket.hpp"
#include "../src/globals.hpp"
#include "../src/log.hpp"
#include "../src/sdk/CProtoBufMsgBase.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <sys/resource.h>
static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)
namespace
{
std::string readAll(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
// Runs `save` with writes capped at 16 bytes.
template<typename F> bool saveOverLimit(F save)
{
	rlimit previous{};
	getrlimit(RLIMIT_FSIZE, &previous);
	rlimit tiny = previous;
	tiny.rlim_cur = 16;
	setrlimit(RLIMIT_FSIZE, &tiny);
	const bool saved = save();
	setrlimit(RLIMIT_FSIZE, &previous);
	return saved;
}
}
int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	char home[] = "/tmp/test_ticket_cache_XXXXXX";
	if (!mkdtemp(home)) return 2;
	setenv("HOME", home, 1);
	setenv("XDG_CONFIG_HOME", home, 1);
	g_pLog = std::unique_ptr<CLog>(new CLog((std::string(home) + "/test.log").c_str()));
	g_currentSteamId = CSteamId(76561198000000001ULL);
	signal(SIGXFSZ, SIG_IGN);

	CMsgClientGetAppOwnershipTicketResponse ticket;
	ticket.set_app_id(42);
	ticket.set_ticket("first ownership ticket");
	CHECK(Ticket::saveTicketToCache(&ticket), "an ownership ticket is saved");
	const std::string ticketPath = Ticket::getTicketPath(42);
	const std::string savedTicket = readAll(ticketPath);
	ticket.set_ticket("second ownership ticket, a longer one");
	CHECK(!saveOverLimit([&] { return Ticket::saveTicketToCache(&ticket); }),
	      "a failed ownership ticket write is reported");
	CHECK(!savedTicket.empty() && readAll(ticketPath) == savedTicket,
	      "a failed ownership ticket write leaves the saved file intact");

	CMsgClientRequestEncryptedAppTicketResponse encrypted;
	encrypted.set_app_id(43);
	encrypted.set_eresult(1);
	CHECK(Ticket::saveEncryptedTicketToCache(&encrypted), "an encrypted ticket is saved");
	const std::string encryptedPath = Ticket::getEncryptedTicketPath(43);
	const std::string savedEncrypted = readAll(encryptedPath);
	encrypted.mutable_encrypted_app_ticket()->set_ticket_version_no(2);
	encrypted.mutable_encrypted_app_ticket()->set_encrypted_ticket(std::string(64, 'x'));
	CHECK(!saveOverLimit([&] { return Ticket::saveEncryptedTicketToCache(&encrypted); }),
	      "a failed encrypted ticket write is reported");
	CHECK(!savedEncrypted.empty() && readAll(encryptedPath) == savedEncrypted,
	      "a failed encrypted ticket write leaves the saved file intact");

	g_pLog.reset();
	std::filesystem::remove_all(home);
	return g_failures == 0 ? 0 : 1;
}
