#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#if __has_include("../src/runtime_attestation.hpp")
#include "../src/runtime_attestation.hpp"

static int failures = 0;

static void expectEqual(const std::string& actual, const std::string& expected)
{
	if (actual == expected)
		return;
	std::cerr << "expected: " << expected << "\nactual:   " << actual << "\n";
	++failures;
}

static void expectTrue(bool actual, const char* message)
{
	if (actual)
		return;
	std::cerr << message << "\n";
	++failures;
}

static void expectContains(const std::string& actual, const std::string& expected,
	                       const char* message)
{
	if (actual.find(expected) != std::string::npos)
		return;
	std::cerr << message << "\nmissing: " << expected << "\nactual:  " << actual << "\n";
	++failures;
}

int main()
{
	const RuntimeAttestation::Metadata metadata
	{
		"session-123",
		"candidate-build",
		4242,
		9001,
	};
	const std::string line = RuntimeAttestation::encodeEvent
	(
		metadata,
		"locator-resolved",
		{
			RuntimeAttestation::Field::text("symbol", "Patterns::A\\\"B"),
			RuntimeAttestation::Field::number("target_rva", 4660),
			RuntimeAttestation::Field::boolean("executable", true),
		}
	);
	expectEqual
	(
		line,
		"{\"schema\":1,\"event\":\"locator-resolved\","
		"\"session\":\"session-123\",\"pid\":4242,\"monotonic_ms\":9001,"
		"\"candidate_build_id\":\"candidate-build\","
		"\"symbol\":\"Patterns::A\\\\\\\"B\",\"target_rva\":4660,"
		"\"executable\":true}"
	);

	const std::string path = "/tmp/slssteam-attestation-test-"
	                       + std::to_string(getpid()) + ".jsonl";
	unlink(path.c_str());
	{
		RuntimeAttestation::EventWriter writer(path);
		expectTrue(writer.good(), "event writer did not open the output file");
		expectTrue(writer.append(metadata, "module-loaded"),
		           "event writer did not append an event");
		expectTrue(writer.appendOnce("remote-storage", metadata, "hook-invoked"),
		           "event writer did not append the first once-only event");
		expectTrue(writer.appendOnce("remote-storage", metadata, "hook-invoked"),
		           "event writer treated a duplicate once-only event as an error");
	}

	std::ifstream input(path);
	const std::string contents((std::istreambuf_iterator<char>(input)), {});
	expectEqual
	(
		contents,
		RuntimeAttestation::encodeEvent(metadata, "module-loaded") + "\n"
		+ RuntimeAttestation::encodeEvent(metadata, "hook-invoked") + "\n"
	);
	struct stat fileStat {};
	expectTrue(stat(path.c_str(), &fileStat) == 0, "event file is missing");
	expectTrue((fileStat.st_mode & 0777) == 0600,
	           "event file permissions are not restricted to 0600");
	unlink(path.c_str());

	const std::string sessionPath = "/tmp/slssteam-attestation-session-test-"
	                              + std::to_string(getpid()) + ".jsonl";
	unlink(sessionPath.c_str());
	setenv("SLSSTEAM_ATTESTATION_FILE", sessionPath.c_str(), 1);
	setenv("SLSSTEAM_ATTESTATION_SESSION", "ci-session", 1);
	expectTrue(RuntimeAttestation::initialize("global-build"),
	           "environment-backed attestation did not initialize");
	expectTrue
	(
		RuntimeAttestation::emit
		(
			"runtime-ready",
			{RuntimeAttestation::Field::text("module", "steamclient.so")}
		),
		"environment-backed attestation did not emit"
	);
	std::ifstream sessionInput(sessionPath);
	const std::string sessionContents
	(
		(std::istreambuf_iterator<char>(sessionInput)), {}
	);
	expectContains(sessionContents, "\"event\":\"attestation-started\"",
	               "attestation start event is missing");
	expectContains(sessionContents, "\"event\":\"runtime-ready\"",
	               "runtime event is missing");
	expectContains(sessionContents, "\"session\":\"ci-session\"",
	               "session identity is missing");
	expectContains(sessionContents, "\"candidate_build_id\":\"global-build\"",
	               "candidate build identity is missing");
	expectContains(sessionContents, "\"module\":\"steamclient.so\"",
	               "runtime event fields are missing");
	unlink(sessionPath.c_str());

	if (failures != 0)
		return 1;
	std::cout << "runtime attestation tests passed\n";
	return 0;
}
#else
int main()
{
	std::cerr << "runtime_attestation.hpp is missing\n";
	return 1;
}
#endif
