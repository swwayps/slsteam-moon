#include "pattern-refresh/catalog.hpp"

#include <openssl/evp.h>

#include <arpa/inet.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


namespace
{
	int failures = 0;

	void expect(bool condition, std::string_view message)
	{
		if (condition)
			return;
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}

	PatternRefresh::Response ok(std::string body)
	{
		return {true, 200, std::move(body), {}};
	}

	PatternRefresh::Response status(long value)
	{
		return {true, value, {}, {}};
	}

	PatternRefresh::Response transportFailure()
	{
		return {false, 0, {}, {}};
	}

	PatternRefresh::Response notModified()
	{
		return {true, 304, {}, {}};
	}

	PatternRefresh::ResponsePair pair(
		PatternRefresh::Response catalog,
		PatternRefresh::Response signature
	)
	{
		return {std::move(catalog), std::move(signature)};
	}

	bool valid(std::string_view catalog, std::string_view signature)
	{
		return catalog == "valid-catalog" && signature == "valid-signature";
	}

	struct SigningKey
	{
		std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
		PatternRefresh::PublicKey publicKey{};
	};

	SigningKey makeSigningKey()
	{
		std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
			EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free
		);
		EVP_PKEY* raw = nullptr;
		expect(context && EVP_PKEY_keygen_init(context.get()) == 1,
		       "Ed25519 key generation initializes");
		expect(context && EVP_PKEY_keygen(context.get(), &raw) == 1,
		       "ephemeral Ed25519 key is generated");
		SigningKey result;
		result.key.reset(raw);
		size_t publicSize = result.publicKey.size();
		expect(result.key && EVP_PKEY_get_raw_public_key(
			result.key.get(), result.publicKey.data(), &publicSize) == 1,
			"raw Ed25519 public key is extracted"
		);
		expect(publicSize == result.publicKey.size(), "Ed25519 public key is 32 bytes");
		return result;
	}

	std::vector<unsigned char> sign(EVP_PKEY* key, std::string_view body)
	{
		std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
			EVP_MD_CTX_new(), EVP_MD_CTX_free
		);
		expect(context && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) == 1,
		       "Ed25519 signer initializes");
		size_t size = 0;
		expect(EVP_DigestSign(context.get(), nullptr, &size,
		                      reinterpret_cast<const unsigned char*>(body.data()), body.size()) == 1,
		       "Ed25519 signature size is available");
		std::vector<unsigned char> signature(size);
		expect(EVP_DigestSign(context.get(), signature.data(), &size,
		                      reinterpret_cast<const unsigned char*>(body.data()), body.size()) == 1,
		       "Ed25519 signature is generated");
		signature.resize(size);
		return signature;
	}

	PatternRefresh::ExactModule exactModule()
	{
		return
		{
			"steamclient",
			"steamclient.so",
			std::string(64, 'a'),
			8192,
			std::string(40, 'b'),
			{{0, 8192}},
		};
	}

	std::string canonicalToml(
		std::uint64_t revision,
		const PatternRefresh::ExactModule& module = exactModule()
	)
	{
		return
			"schema = 1\n"
			"platform = \"linux32\"\n"
			"component = \"" + module.component + "\"\n"
			"module_name = \"" + module.moduleName + "\"\n"
			"module_sha256 = \"" + module.sha256 + "\"\n"
			"module_size = " + std::to_string(module.size) + "\n"
			"gnu_build_id = \"" + module.gnuBuildId + "\"\n"
			"steam_version = 1785347151\n"
			"consumer_commit = \"" + std::string(40, 'c') + "\"\n"
			"minimum_consumer_schema = 1\n"
			"revision = " + std::to_string(revision) + "\n"
			"source = \"deterministic\"\n"
			"\n"
			"[[locators]]\n"
			"symbol = \"Patterns::Root\"\n"
			"name = \"Root\"\n"
			"target_rva = 128\n"
			"match_rva = 124\n"
			"signature = \"55 89 E5\"\n"
			"follow_mode = \"None\"\n"
			"resolver = \"signature\"\n"
			"required = true\n"
			"match_count = 1\n"
			"target_count = 1\n";
	}

	struct TempDirectory
	{
		std::filesystem::path path;

		TempDirectory()
		{
			std::string pattern = "/tmp/slsteam-pattern-refresh-test-XXXXXX";
			char* created = mkdtemp(pattern.data());
			expect(created != nullptr, "temporary test directory is created");
			if (created != nullptr)
				path = created;
		}

		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};

	void put16(std::vector<unsigned char>& bytes, std::size_t offset, std::uint16_t value)
	{
		bytes.at(offset) = static_cast<unsigned char>(value);
		bytes.at(offset + 1) = static_cast<unsigned char>(value >> 8);
	}

	void put32(std::vector<unsigned char>& bytes, std::size_t offset, std::uint32_t value)
	{
		for (std::size_t index = 0; index < 4; ++index)
			bytes.at(offset + index) = static_cast<unsigned char>(value >> (index * 8));
	}

	std::vector<unsigned char> testElf()
	{
		std::vector<unsigned char> bytes(512, 0);
		std::memcpy(bytes.data(), "\x7f" "ELF", 4);
		bytes[4] = 1;
		bytes[5] = 1;
		bytes[6] = 1;
		put32(bytes, 0x1C, 52);
		put16(bytes, 0x2A, 32);
		put16(bytes, 0x2C, 2);
		put32(bytes, 52, 1);
		put32(bytes, 52 + 4, 0);
		put32(bytes, 52 + 8, 0);
		put32(bytes, 52 + 16, 512);
		put32(bytes, 52 + 20, 512);
		put32(bytes, 52 + 24, 5);
		put32(bytes, 84, 4);
		put32(bytes, 84 + 4, 0x100);
		put32(bytes, 84 + 16, 36);
		put32(bytes, 84 + 20, 36);
		put32(bytes, 0x100, 4);
		put32(bytes, 0x104, 20);
		put32(bytes, 0x108, 3);
		std::memcpy(bytes.data() + 0x10C, "GNU\0", 4);
		for (std::size_t index = 0; index < 20; ++index)
			bytes.at(0x110 + index) = static_cast<unsigned char>(index);
		return bytes;
	}

	void writeBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char*>(bytes.data()),
		             static_cast<std::streamsize>(bytes.size()));
		expect(output.good(), "test bytes are written");
	}

	std::string readFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(input)), {});
	}

	struct Route
	{
		long status = 200;
		std::string body;
		std::string etag;
		long delayMilliseconds = 0;
	};

	class HttpServer
	{
	public:
		explicit HttpServer(std::map<std::string, Route> routes)
			: routes_(std::move(routes))
		{
			listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
			expect(listener_ >= 0, "loopback HTTP socket is created");
			int reuse = 1;
			setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
			sockaddr_in address {};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = 0;
			expect(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
			       "loopback HTTP socket binds");
			expect(listen(listener_, 16) == 0, "loopback HTTP socket listens");
			socklen_t size = sizeof(address);
			expect(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0,
			       "loopback HTTP port is discovered");
			port_ = ntohs(address.sin_port);
			worker_ = std::thread([this] { serve(); });
		}

		~HttpServer()
		{
			stop_.store(true);
			shutdown(listener_, SHUT_RDWR);
			close(listener_);
			if (worker_.joinable())
				worker_.join();
		}

		std::string base(std::string_view prefix) const
		{
			return "http://127.0.0.1:" + std::to_string(port_) + std::string(prefix);
		}

		std::size_t requestCount() const
		{
			std::lock_guard lock(mutex_);
			return requests_.size();
		}

		bool sawConditional(std::string_view path) const
		{
			std::lock_guard lock(mutex_);
			return std::any_of(requests_.begin(), requests_.end(), [&](const Request& request)
			{
				return request.path == path && request.conditional;
			});
		}

	private:
		struct Request
		{
			std::string path;
			bool conditional = false;
		};

		void serve()
		{
			while (!stop_.load())
			{
				pollfd watched {listener_, POLLIN, 0};
				if (poll(&watched, 1, 100) <= 0)
					continue;
				const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
				if (client < 0)
					continue;
				serveOne(client);
				close(client);
			}
		}

		void serveOne(int client)
		{
			std::string request;
			std::array<char, 4096> buffer {};
			while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
			{
				const ssize_t count = recv(client, buffer.data(), buffer.size(), 0);
				if (count <= 0)
					return;
				request.append(buffer.data(), static_cast<std::size_t>(count));
			}
			const std::size_t firstSpace = request.find(' ');
			const std::size_t secondSpace = request.find(' ', firstSpace + 1);
			if (firstSpace == std::string::npos || secondSpace == std::string::npos)
				return;
			const std::string path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
			const auto found = routes_.find(path);
			Route route = found == routes_.end() ? Route{404, {}, {}} : found->second;
			const bool conditional = !route.etag.empty()
				&& request.find("If-None-Match: " + route.etag + "\r\n") != std::string::npos;
			{
				std::lock_guard lock(mutex_);
				requests_.push_back({path, conditional});
			}
			if (conditional && route.status == 200)
			{
				route.status = 304;
				route.body.clear();
			}
			for (long remaining = route.delayMilliseconds;
			     remaining > 0 && !stop_.load(); remaining -= std::min(remaining, 10L))
			{
				std::this_thread::sleep_for(
					std::chrono::milliseconds(std::min(remaining, 10L))
				);
			}
			if (stop_.load())
				return;
			const char* reason = route.status == 200 ? "OK"
			                   : route.status == 304 ? "Not Modified" : "Not Found";
			std::string response = "HTTP/1.1 " + std::to_string(route.status) + " " + reason
				+ "\r\nConnection: close\r\nContent-Length: "
				+ std::to_string(route.body.size()) + "\r\n";
			if (!route.etag.empty())
				response += "ETag: " + route.etag + "\r\n";
			response += "\r\n" + route.body;
			std::size_t sent = 0;
			while (sent < response.size())
			{
				const ssize_t count = send(
					client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL
				);
				if (count <= 0)
					return;
				sent += static_cast<std::size_t>(count);
			}
		}

		std::map<std::string, Route> routes_;
		mutable std::mutex mutex_;
		std::vector<Request> requests_;
		std::atomic<bool> stop_ {false};
		std::thread worker_;
		int listener_ = -1;
		std::uint16_t port_ = 0;
	};
}


int main()
{
	using namespace PatternRefresh;
	const std::string sha(64, 'a');
	const auto github = makeUrls(Mirror::GitHub, "steamclient", sha);
	expect(
		github.catalog
		== "https://raw.githubusercontent.com/swwayps/steam-monitor/main/"
		   "linux32/steamclient/" + sha + ".toml",
		"GitHub raw catalog URL is exact"
	);
	// The producer publishes the detached signature as a sibling of the catalog
	// file itself (`<sha256>.toml.sig`), not as a `<sha256>.sig` peer.  Fetching
	// the wrong name 404s and the consumer then fails closed on every build, so
	// the exact published name is asserted here.
	expect(github.signature == github.catalog + ".sig",
	       "GitHub signature URL is the published <sha256>.toml.sig sibling");
	const auto cdn = makeUrls(Mirror::JsDelivr, "steamui", sha);
	expect(
		cdn.catalog
		== "https://cdn.jsdelivr.net/gh/swwayps/steam-monitor@main/"
		   "linux32/steamui/" + sha + ".toml",
		"jsDelivr catalog URL is exact"
	);
	expect(cdn.signature == cdn.catalog + ".sig",
	       "jsDelivr signature URL is the published <sha256>.toml.sig sibling");

	const ResponsePair good = pair(ok("valid-catalog"), ok("valid-signature"));
	const ResponsePair badBody = pair(ok("invalid-catalog"), ok("valid-signature"));
	const ResponsePair unavailable = pair(transportFailure(), transportFailure());
	const CachedPair cache{"valid-catalog", "valid-signature", true};

	{
		const auto selected = chooseCandidate(good, unavailable, std::nullopt, valid);
		expect(selected.source == Source::GitHub, "valid GitHub response wins");
		expect(selected.catalog == "valid-catalog", "GitHub body is retained");
	}
	{
		const auto selected = chooseCandidate(
			pair(status(404), status(404)), good, std::nullopt, valid
		);
		expect(selected.source == Source::JsDelivr, "GitHub 404 falls back to jsDelivr");
	}
	{
		const auto selected = chooseCandidate(unavailable, good, std::nullopt, valid);
		expect(selected.source == Source::JsDelivr,
		       "GitHub transport failure falls back to jsDelivr");
	}
	{
		const auto selected = chooseCandidate(
			pair(status(503), status(503)), good, std::nullopt, valid
		);
		expect(selected.source == Source::JsDelivr,
		       "GitHub HTTP failure falls back to jsDelivr");
	}
	{
		const auto selected = chooseCandidate(badBody, good, std::nullopt, valid);
		expect(selected.source == Source::JsDelivr,
		       "invalid GitHub content falls back to jsDelivr");
	}
	{
		const auto selected = chooseCandidate(
			pair(notModified(), notModified()), unavailable, cache, valid
		);
		expect(selected.source == Source::RevalidatedCache,
		       "paired GitHub 304 reuses the validated exact-SHA cache");
	}
	{
		const auto selected = chooseCandidate(
			pair(notModified(), ok("valid-signature")), unavailable, cache, valid
		);
		expect(selected.source == Source::GitHub,
		       "mixed 304/200 response is reconstructed from exact cache");
	}
	{
		const auto selected = chooseCandidate(unavailable, badBody, cache, valid);
		expect(selected.source == Source::OfflineCache,
		       "validated exact-SHA cache is the final source");
	}
	{
		CachedPair wrongSha = cache;
		wrongSha.exactSha = false;
		const auto selected = chooseCandidate(unavailable, badBody, wrongSha, valid);
		expect(selected.source == Source::None,
		       "cache for a different SHA is never reused");
	}
	{
		CachedPair invalidCache{"invalid-catalog", "valid-signature", true};
		const auto selected = chooseCandidate(unavailable, unavailable, invalidCache, valid);
		expect(selected.source == Source::None, "invalid cache is never activated");
	}

	const SigningKey signing = makeSigningKey();
	const std::string revisionOne = canonicalToml(1);
	const auto signatureOne = sign(signing.key.get(), revisionOne);
	std::string validationError;
	const auto acceptedOne = validateSignedCatalog(
		revisionOne, signatureOne, exactModule(), signing.publicKey, &validationError
	);
	expect(acceptedOne.has_value(), "valid signed exact-module catalog is accepted");
	expect(validationError.empty(), "valid signed catalog has no diagnostic");
	if (acceptedOne)
		expect(acceptedOne->revision == 1, "validated revision is retained");

	{
		auto wrongSignature = signatureOne;
		wrongSignature.front() ^= 0x80;
		expect(!validateSignedCatalog(
			revisionOne, wrongSignature, exactModule(), signing.publicKey
		), "wrong Ed25519 signature is rejected");
	}
	{
		auto oversizedSignature = signatureOne;
		oversizedSignature.push_back(0);
		expect(!validateSignedCatalog(
			revisionOne, oversizedSignature, exactModule(), signing.publicKey
		), "signature not exactly 64 bytes is rejected");
	}
	{
		auto wrongIdentity = exactModule();
		wrongIdentity.sha256.assign(64, 'd');
		expect(!validateSignedCatalog(
			revisionOne, signatureOne, wrongIdentity, signing.publicKey
		), "catalog for another module SHA is rejected");
	}
	{
		auto wrongSize = exactModule();
		++wrongSize.size;
		expect(!validateSignedCatalog(
			revisionOne, signatureOne, wrongSize, signing.publicKey
		), "catalog for another module size is rejected");
	}
	{
		auto wrongBuildId = exactModule();
		wrongBuildId.gnuBuildId.assign(40, 'd');
		expect(!validateSignedCatalog(
			revisionOne, signatureOne, wrongBuildId, signing.publicKey
		), "catalog for another GNU build ID is rejected");
	}
	{
		auto nonExecutable = exactModule();
		nonExecutable.executableRanges = {{0, 64}};
		expect(!validateSignedCatalog(
			revisionOne, signatureOne, nonExecutable, signing.publicKey
		), "catalog target outside executable ELF segments is rejected");
	}
	{
		const std::string oversized(PatternRefresh::kMaximumCatalogSize + 1, 'x');
		expect(!validateSignedCatalog(
			oversized, signatureOne, exactModule(), signing.publicKey
		), "catalog over 256 KiB is rejected before activation");
	}
	{
		const std::string revisionTwo = canonicalToml(2);
		const auto signatureTwo = sign(signing.key.get(), revisionTwo);
		const auto acceptedTwo = validateSignedCatalog(
			revisionTwo, signatureTwo, exactModule(), signing.publicKey
		);
		expect(acceptedTwo.has_value(), "higher signed revision validates");
		if (acceptedOne && acceptedTwo)
		{
			expect(acceptsRevision(*acceptedTwo, revisionTwo, acceptedOne, revisionOne),
			       "higher revision is accepted");
			expect(!acceptsRevision(*acceptedOne, revisionOne, acceptedTwo, revisionTwo),
			       "lower revision rollback is rejected");
			expect(acceptsRevision(*acceptedOne, revisionOne, acceptedOne, revisionOne),
			       "identical same revision is idempotent");
			expect(!acceptsRevision(*acceptedOne, revisionOne, acceptedOne,
			                        revisionOne + "#"),
			       "different bytes at the same revision are rejected");
		}
	}

	{
		TempDirectory temporary;
		const auto modulePath = temporary.path / "steamclient.so";
		const auto statePath = temporary.path / "module.state";
		writeBytes(modulePath, testElf());
		std::string error;
		const auto first = inspectModule(
			"steamclient", "steamclient.so", modulePath, statePath, {}, &error
		);
		expect(first.has_value(), "ELF identity is inspected");
		expect(error.empty(), "valid ELF inspection has no diagnostic");
		if (first)
		{
			expect(!first->fromStatCache, "first module inspection hashes the ELF");
			expect(first->identity.size == 512, "module size is retained");
			expect(first->identity.sha256.size() == 64, "module SHA-256 is retained");
			expect(first->identity.gnuBuildId
			       == "000102030405060708090a0b0c0d0e0f10111213",
			       "GNU build ID is retained");
			expect(first->identity.executableRanges.size() == 1
			       && first->identity.executableRanges[0].begin == 0
			       && first->identity.executableRanges[0].end == 512,
			       "executable PT_LOAD range is retained");
		}
		struct stat stateStat {};
		expect(stat(statePath.c_str(), &stateStat) == 0, "digest stat cache is written");
		expect((stateStat.st_mode & 0777) == 0600, "digest stat cache mode is 0600");
		const auto second = inspectModule(
			"steamclient", "steamclient.so", modulePath, statePath
		);
		expect(second.has_value() && second->fromStatCache,
		       "unchanged module reuses the digest stat cache");

		const auto mutationState = temporary.path / "mutated.state";
		const auto mutated = inspectModule(
			"steamclient", "steamclient.so", modulePath, mutationState,
			[&]
			{
				std::fstream stream(modulePath, std::ios::binary | std::ios::in | std::ios::out);
				stream.seekp(511);
				stream.put(static_cast<char>(0x7F));
				stream.flush();
				std::filesystem::last_write_time(
					modulePath,
					std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2)
				);
			},
			&error
		);
		expect(!mutated.has_value(), "module mutation between hash and re-stat is rejected");
		expect(!std::filesystem::exists(mutationState),
		       "mutated module does not publish a digest cache entry");
	}

	{
		TempDirectory temporary;
		const auto root = temporary.path / "patterns";
		std::string error;
		const auto activatedOne = activateSignedCatalog(
			root, exactModule(), revisionOne, signatureOne, signing.publicKey, &error
		);
		expect(activatedOne.active && activatedOne.changed && activatedOne.revision == 1,
		       "first valid signed catalog is activated");
		const auto catalogPath = root / "steamclient" / (std::string(64, 'a') + ".toml");
		const auto signaturePath = root / "steamclient" / (std::string(64, 'a') + ".sig");
		expect(readFile(catalogPath) == revisionOne, "activated TOML bytes are exact");
		expect(readFile(signaturePath).size() == 64, "activated signature bytes are exact");
		struct stat catalogStat {};
		struct stat signatureStat {};
		expect(stat(catalogPath.c_str(), &catalogStat) == 0
		       && (catalogStat.st_mode & 0777) == 0600,
		       "active TOML mode is 0600");
		expect(stat(signaturePath.c_str(), &signatureStat) == 0
		       && (signatureStat.st_mode & 0777) == 0600,
		       "active signature mode is 0600");

		auto invalidSignature = signatureOne;
		invalidSignature.front() ^= 1;
		const auto rejected = activateSignedCatalog(
			root, exactModule(), canonicalToml(2), invalidSignature,
			signing.publicKey, &error
		);
		expect(!rejected.active, "invalid candidate is rejected before activation");
		expect(readFile(catalogPath) == revisionOne,
		       "invalid candidate never overwrites valid active TOML");

		const std::string revisionTwo = canonicalToml(2);
		const auto signatureTwo = sign(signing.key.get(), revisionTwo);
		const auto activatedTwo = activateSignedCatalog(
			root, exactModule(), revisionTwo, signatureTwo, signing.publicKey, &error
		);
		expect(activatedTwo.active && activatedTwo.changed && activatedTwo.revision == 2,
		       "higher signed revision replaces active metadata");
		const auto rollback = activateSignedCatalog(
			root, exactModule(), revisionOne, signatureOne, signing.publicKey, &error
		);
		expect(!rollback.active, "lower signed revision is rejected");
		expect(readFile(catalogPath) == revisionTwo,
		       "rollback rejection preserves higher active revision");
	}

	{
		TempDirectory temporary;
		const std::string catalog = canonicalToml(1);
		const auto signature = sign(signing.key.get(), catalog);
		const std::string signatureBody(
			reinterpret_cast<const char*>(signature.data()), signature.size()
		);
		const std::string sha(64, 'a');
		const std::string catalogPath = "/linux32/steamclient/" + sha + ".toml";
		const std::string signaturePath = catalogPath + ".sig";
		HttpServer server(
			{
				{"/github" + catalogPath, {404, {}, {}}},
				{"/github" + signaturePath, {404, {}, {}}},
				{"/cdn" + catalogPath, {200, catalog, "\"catalog-v1\""}},
				{"/cdn" + signaturePath, {200, signatureBody, "\"signature-v1\""}},
			}
		);
		const MirrorBases mirrors
		{
			server.base("/github/"),
			server.base("/cdn/"),
			true,
		};
		std::string error;
		const auto first = refreshComponent(
			temporary.path / "patterns", exactModule(), signing.publicKey,
			RefreshMode::Remote, mirrors, &error
		);
		expect(first.active && first.changed && first.source == Source::JsDelivr,
		       "real HTTP 404 falls back and activates jsDelivr bytes");
		expect(server.requestCount() == 4,
		       "primary and fallback catalog/signature pairs are fetched");

		const auto second = refreshComponent(
			temporary.path / "patterns", exactModule(), signing.publicKey,
			RefreshMode::Remote, mirrors, &error
		);
		expect(second.active && !second.changed && second.source == Source::RevalidatedCache,
		       "ETag 304 revalidates the exact signed cache");
		expect(server.requestCount() == 8, "second refresh performs bounded conditional requests");
		expect(server.sawConditional("/cdn" + catalogPath),
		       "cached catalog ETag is sent to jsDelivr");
		expect(server.sawConditional("/cdn" + signaturePath),
		       "cached signature ETag is sent to jsDelivr");
		struct stat etagStat {};
		const auto etagFile = temporary.path / "patterns/.state/steamclient"
			/ (sha + ".jsdelivr.catalog.etag");
		expect(stat(etagFile.c_str(), &etagStat) == 0
		       && (etagStat.st_mode & 0777) == 0600,
		       "ETag state is restricted to mode 0600");
	}

	{
		TempDirectory temporary;
		const auto patternRoot = temporary.path / "patterns";
		const auto activated = activateSignedCatalog(
			patternRoot, exactModule(), revisionOne, signatureOne, signing.publicKey
		);
		expect(activated.active, "cache-only fixture activates exact signed metadata");
		HttpServer server({});
		const MirrorBases mirrors
		{
			server.base("/github/"),
			server.base("/cdn/"),
			true,
		};
		const auto started = std::chrono::steady_clock::now();
		const auto refreshed = refreshComponent(
			patternRoot, exactModule(), signing.publicKey,
			RefreshMode::CacheOnly, mirrors
		);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started
		).count();
		expect(refreshed.active && refreshed.source == Source::OfflineCache,
		       "cache-only refresh accepts the signed exact-module cache");
		expect(server.requestCount() == 0,
		       "cache-only refresh performs no HTTP requests");
		expect(elapsed < 250, "cache-only refresh returns without a network deadline");
	}

	{
		TempDirectory temporary;
		const std::string catalog = canonicalToml(1);
		const auto signature = sign(signing.key.get(), catalog);
		const std::string signatureBody(
			reinterpret_cast<const char*>(signature.data()), signature.size()
		);
		const std::string sha(64, 'a');
		const std::string stem = "/linux32/steamclient/" + sha;
		HttpServer fast(
			{
				{stem + ".toml", {200, catalog, "\"fast-catalog\""}},
				{stem + ".toml.sig", {200, signatureBody, "\"fast-signature\""}},
			}
		);
		HttpServer stalled(
			{
				{stem + ".toml", {200, catalog, {}, 5000}},
				{stem + ".toml.sig", {200, signatureBody, {}, 5000}},
			}
		);
		const MirrorBases mirrors
		{
			fast.base("/"),
			stalled.base("/"),
			true,
		};
		const auto started = std::chrono::steady_clock::now();
		const auto refreshed = refreshComponent(
			temporary.path / "patterns", exactModule(), signing.publicKey,
			RefreshMode::Remote, mirrors
		);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started
		).count();
		expect(refreshed.active && refreshed.source == Source::GitHub,
		       "first complete valid signed mirror wins");
		expect(elapsed < 1500,
		       "a stalled secondary mirror does not hold a valid primary mirror");
	}

	{
		const auto parsedKey = parsePublicKeyHex(std::string(64, 'a'));
		expect(parsedKey.has_value(), "64 lowercase hexadecimal public-key bytes parse");
		expect(parsedKey && parsedKey->front() == 0xAA && parsedKey->back() == 0xAA,
		       "public-key hexadecimal decoding is exact");
		expect(!parsePublicKeyHex(std::string(63, 'a')),
		       "short public key is rejected");
		expect(!parsePublicKeyHex(std::string(64, 'A')),
		       "noncanonical uppercase public key is rejected");
	}
	{
		const std::vector<std::string_view> arguments
		{
			"pattern-refresh", "--steam-root", "/steam",
			"--config-root", "/config",
		};
		const auto invocation = parseInvocation(arguments);
		expect(invocation.has_value(), "exact refresh invocation parses");
		expect(invocation && invocation->steamRoot == "/steam"
		       && invocation->configRoot == "/config"
		       && invocation->mode == RefreshMode::Remote,
		       "refresh invocation retains exact absolute roots");
		const auto cacheOnly = parseInvocation(
			{"pattern-refresh", "--cache-only", "--steam-root", "/steam",
			 "--config-root", "/config"}
		);
		expect(cacheOnly && cacheOnly->mode == RefreshMode::CacheOnly,
		       "cache-only refresh invocation parses explicitly");
		expect(!parseInvocation(
			{"pattern-refresh", "--cache-only", "--cache-only",
			 "--steam-root", "/steam", "--config-root", "/config"}
		), "duplicate cache-only option is rejected");
		auto duplicate = arguments;
		duplicate.insert(duplicate.end(), {"--steam-root", "/other"});
		expect(!parseInvocation(duplicate), "duplicate refresh option is rejected");
		auto unknown = arguments;
		unknown.insert(unknown.end(), {"--other", "/value"});
		expect(!parseInvocation(unknown), "unknown refresh option is rejected");
		expect(!parseInvocation(
			{"pattern-refresh", "--steam-root", "relative", "--config-root", "/config"}
		), "relative refresh root is rejected");
		expect(!parseInvocation(
			{"pattern-refresh", "--steam-root", "/steam"}
		), "missing refresh option is rejected");
	}

	{
		TempDirectory temporary;
		const auto steamRoot = temporary.path / "steam";
		const auto configRoot = temporary.path / "config";
		std::filesystem::create_directories(steamRoot / "ubuntu12_32");
		writeBytes(steamRoot / "ubuntu12_32/steamclient.so", testElf());
		writeBytes(steamRoot / "ubuntu12_32/steamui.so", testElf());
		const auto client = inspectModule(
			"steamclient", "steamclient.so",
			steamRoot / "ubuntu12_32/steamclient.so",
			temporary.path / "client.state"
		);
		const auto ui = inspectModule(
			"steamui", "steamui.so",
			steamRoot / "ubuntu12_32/steamui.so",
			temporary.path / "ui.state"
		);
		expect(client.has_value() && ui.has_value(), "both test Steam modules are inspected");
		if (client && ui)
		{
			const std::string clientBody = canonicalToml(1, client->identity);
			const std::string uiBody = canonicalToml(1, ui->identity);
			const auto clientSignature = sign(signing.key.get(), clientBody);
			const auto uiSignature = sign(signing.key.get(), uiBody);
			const auto binary = [](const std::vector<unsigned char>& value)
			{
				return std::string(reinterpret_cast<const char*>(value.data()), value.size());
			};
			const std::string clientStem = "/linux32/steamclient/" + client->identity.sha256;
			const std::string uiStem = "/linux32/steamui/" + ui->identity.sha256;
			HttpServer server(
				{
					{"/github" + clientStem + ".toml", {200, clientBody, "\"c1\""}},
					{"/github" + clientStem + ".toml.sig",
					 {200, binary(clientSignature), "\"c2\""}},
					{"/github" + uiStem + ".toml", {200, uiBody, "\"u1\""}},
					{"/github" + uiStem + ".toml.sig",
					 {200, binary(uiSignature), "\"u2\""}},
				}
			);
			const MirrorBases mirrors
			{
				server.base("/github/"),
				server.base("/cdn/"),
				true,
			};
			std::string error;
			const int result = refreshInstallation(
				steamRoot, configRoot, signing.publicKey,
				RefreshMode::Remote, mirrors, &error
			);
			if (result != 0)
				std::cerr << "refreshInstallation diagnostic: " << error << '\n';
			expect(result == 0, "both exact Steam modules refresh successfully");
			expect(error.empty(), "successful installation refresh has no diagnostic");
			expect(server.requestCount() >= 4 && server.requestCount() <= 8,
			       "both modules refresh while redundant mirror requests may be cancelled");
			expect(std::filesystem::exists(
				configRoot / "SLSsteam/patterns/steamclient" / (client->identity.sha256 + ".toml")
			), "steamclient active TOML is installed");
			expect(std::filesystem::exists(
				configRoot / "SLSsteam/patterns/steamui" / (ui->identity.sha256 + ".toml")
			), "steamui active TOML is installed");
			const std::size_t requestsBeforeCacheOnly = server.requestCount();
			expect(refreshInstallation(
				steamRoot, configRoot, signing.publicKey,
				RefreshMode::CacheOnly, mirrors
			) == 0, "installation cache-only mode accepts both exact signed catalogs");
			expect(server.requestCount() == requestsBeforeCacheOnly,
			       "installation cache-only mode performs no HTTP requests");
		}
		expect(refreshInstallation(
			temporary.path / "relative/../missing", configRoot,
			signing.publicKey, RefreshMode::Remote
		) == 2, "invalid Steam root returns invocation status 2");
	}

	{
		TempDirectory temporary;
		const auto steamRoot = temporary.path / "steam";
		const auto configRoot = temporary.path / "config";
		const auto stateRoot = configRoot / "SLSsteam/patterns/.state";
		std::filesystem::create_directories(steamRoot / "ubuntu12_32");
		std::filesystem::create_directories(stateRoot);
		writeBytes(steamRoot / "ubuntu12_32/steamclient.so", testElf());
		writeBytes(steamRoot / "ubuntu12_32/steamui.so", testElf());
		const auto lockPath = stateRoot / ".refresh.lock";
		const int lockDescriptor = open(
			lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600
		);
		expect(lockDescriptor >= 0 && flock(lockDescriptor, LOCK_EX | LOCK_NB) == 0,
		       "refresh lock fixture is acquired");
		HttpServer server({});
		const MirrorBases mirrors
		{
			server.base("/github/"),
			server.base("/cdn/"),
			true,
		};
		const auto started = std::chrono::steady_clock::now();
		const int result = refreshInstallation(
			steamRoot, configRoot, signing.publicKey,
			RefreshMode::Remote, mirrors
		);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started
		).count();
		expect(result == 3, "concurrent remote refresh yields to the active updater");
		expect(server.requestCount() == 0,
		       "concurrent remote refresh does not duplicate network work");
		expect(elapsed < 250, "concurrent remote refresh returns immediately");
		if (lockDescriptor >= 0)
			close(lockDescriptor);
	}

	{
		TempDirectory temporary;
		const auto steamRoot = temporary.path / "steam";
		const auto configRoot = temporary.path / "config";
		std::filesystem::create_directories(steamRoot / "ubuntu12_32");
		writeBytes(steamRoot / "ubuntu12_32/steamclient.so", testElf());
		writeBytes(steamRoot / "ubuntu12_32/steamui.so", testElf());
		const auto identity = inspectModule(
			"steamclient", "steamclient.so",
			steamRoot / "ubuntu12_32/steamclient.so",
			temporary.path / "deadline.state"
		);
		expect(identity.has_value(), "deadline fixture module is inspected");
		if (identity)
		{
			const std::string sha = identity->identity.sha256;
			std::map<std::string, Route> routes;
			for (const std::string mirror : {"/github", "/cdn"})
			{
				for (const std::string component : {"steamclient", "steamui"})
				{
					const std::string stem = mirror + "/linux32/" + component + "/" + sha;
					routes.emplace(stem + ".toml", Route{200, "late", {}, 5000});
					routes.emplace(stem + ".toml.sig",
					               Route{200, std::string(64, 'x'), {}, 5000});
				}
			}
			HttpServer server(std::move(routes));
			const MirrorBases mirrors
			{
				server.base("/github/"),
				server.base("/cdn/"),
				true,
			};
			const auto started = std::chrono::steady_clock::now();
			const int result = refreshInstallation(
				steamRoot, configRoot, signing.publicKey,
				RefreshMode::Remote, mirrors
			);
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started
			).count();
			expect(result == 3, "mirror timeout degrades to embedded fallback");
			expect(elapsed >= 3500 && elapsed < 5000,
			       "both modules and mirrors share a roughly four-second deadline");
		}
	}

	if (failures != 0)
	{
		std::cerr << failures << " pattern refresh test(s) failed\n";
		return 1;
	}
	std::cout << "pattern refresh decision tests passed\n";
	return 0;
}
