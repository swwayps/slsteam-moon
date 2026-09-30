#include "../src/feats/achievements.hpp"
#include "../src/feats/stats_policy.hpp"
#include <cstdio>
#include <thread>

int main()
{
	int failures = 0;
	auto check = [&](bool ok, const char* label) {
		std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
		if (!ok) ++failures;
	};
	StatsPolicy::Store store;
	check(!store.localEpoch(7, 123), "unknown account is not local");
	store.setAccount(7);
	auto context = store.context();
	store.observe(7, 123, false, -1, false, false, context.epoch);
	check(!store.localEpoch(7, 123), "failed lookup stays unknown");
	store.observe(7, 123, true, 0, true, false, context.epoch);
	check(store.localEpoch(7, 123) == context.epoch, "native observation permits local package zero");
	check(!store.hasNativeLicense(7, 123),
	      "package zero is not native ownership evidence");
	check(!store.localEpoch(8, 123), "an observation cannot cross accounts");
	store.observe(7, 123, true, 25, true, false, context.epoch);
	store.observe(7, 123, true, 0, true, false, context.epoch);
	check(!store.localEpoch(7, 123), "a real package wins over package zero until license invalidation");
	check(store.hasNativeLicense(7, 123),
	      "a real package records native ownership for manifest routing");
	store.invalidate();
	check(!store.hasNativeLicense(7, 123),
	      "license invalidation clears native ownership evidence");
	StatsPolicy::Store packageStore;
	packageStore.replacePackageApps(packageStore.context(), {123});
	packageStore.setAccount(7);
	check(packageStore.hasNativeLicense(7, 123),
	      "a package snapshot taken before account discovery becomes native evidence");
	packageStore.setAccount(8);
	check(!packageStore.hasNativeLicense(8, 123),
	      "package evidence cannot cross accounts");
	StatsPolicy::Store preAccountSnapshot;
	const auto preAccountContext = preAccountSnapshot.context();
	preAccountSnapshot.replacePackageApps(preAccountContext, {123, 456});
	preAccountSnapshot.replacePackageApps(preAccountContext, {456});
	preAccountSnapshot.setAccount(7);
	check(!preAccountSnapshot.hasNativeLicense(7, 123) &&
	      preAccountSnapshot.hasNativeLicense(7, 456),
	      "pre-account package snapshots retain only the latest license set");
	auto stalePackageContext = packageStore.context();
	packageStore.setAccount(9);
	packageStore.replacePackageApps(stalePackageContext, {456});
	check(!packageStore.hasNativeLicense(9, 456),
	      "stale package publication cannot cross account epochs");
	auto currentPackageContext = packageStore.context();
	packageStore.replacePackageApps(currentPackageContext, {123, 456});
	check(packageStore.hasNativeLicense(9, 456),
	      "current package publication records native ownership");
	packageStore.replacePackageApps(currentPackageContext, {456, 789});
	check(!packageStore.hasNativeLicense(9, 123) &&
	      packageStore.hasNativeLicense(9, 456) &&
	      packageStore.hasNativeLicense(9, 789),
	      "package snapshot replacement revokes removed native evidence");
	store.observe(7, 123, true, 0, true, false, context.epoch);
	check(!store.localEpoch(7, 123), "in-flight observation cannot republish after invalidation");
	context = store.context();
	store.observe(7, 123, true, 0, true, true, context.epoch);
	check(!store.localEpoch(7, 123), "expired package zero is not local evidence");
	store.observe(7, 123, true, 0, true, false, context.epoch);
	store.setAccount(8); store.setAccount(7);
	check(!store.localEpoch(7, 123), "switching away and back clears license evidence");
	check(StatsPolicy::isSelf(0, 7) && StatsPolicy::isSelf(0x0110000100000007ULL, 7),
	      "explicit and implicit self queries are recognized");
	check(!StatsPolicy::isSelf(0, 0) && !StatsPolicy::isSelf(0x0110000100000008ULL, 7),
	      "unknown account and other-user queries are rejected");
	{
		using Achievements::OwnerSource;
		using Achievements::OwnerVerdict;
		constexpr uint64_t base = 0x0110000100000000ULL;
		constexpr uint64_t self = base | 7, fallback = base | 21,
		                   first = base | 31, second = base | 32;
		constexpr uint64_t cooldown = Achievements::kOwnerLookupCooldownSecs;
		Achievements::OwnerDirectory owners;
		auto choice = owners.choose(10, 42, fallback, self, true, 0);
		check(choice.owner == 42 && choice.source == OwnerSource::Override && !choice.lookup,
		      "a per-game override wins");
		owners.report(10, 42, OwnerSource::Override, OwnerVerdict::Bad);
		check(owners.choose(10, 42, fallback, self, true, 0).owner == 42,
		      "a failing override is never replaced");
		owners.report(14, 42, OwnerSource::Override, OwnerVerdict::Good);
		check(owners.choose(14, 0, fallback, self, true, 0).source == OwnerSource::Fallback,
		      "an override's replies do not outlive the override");
		choice = owners.choose(11, 0, fallback, self, true, 0);
		check(choice.owner == fallback && choice.source == OwnerSource::Fallback && !choice.lookup,
		      "the configured fallback needs no lookup");
		owners.report(11, fallback, OwnerSource::Fallback, OwnerVerdict::Bad);
		choice = owners.choose(11, 0, fallback, self, true, 100);
		check(!choice.owner && choice.lookup, "a failed fallback starts one lookup");
		check(!owners.choose(11, 0, fallback, self, true, 101).lookup,
		      "a lookup in flight is not repeated");
		owners.offer(11, {self, first, second});
		choice = owners.choose(11, 0, fallback, self, true, 102);
		check(choice.owner == first && choice.source == OwnerSource::Candidate,
		      "reviewers are tried in page order and never as self");
		owners.report(11, first, OwnerSource::Candidate, OwnerVerdict::Bad);
		check(owners.choose(11, 0, fallback, self, true, 103).owner == second,
		      "a failed reviewer is skipped");
		owners.report(11, second, OwnerSource::Candidate, OwnerVerdict::Good);
		choice = owners.choose(11, 0, fallback, self, true, 104);
		check(choice.owner == second && choice.source == OwnerSource::Preferred,
		      "the owner that answered is reused");
		owners.report(11, second, OwnerSource::Preferred, OwnerVerdict::Unknown);
		check(owners.choose(11, 0, fallback, self, true, 105).owner == second,
		      "an inconclusive reply keeps the owner");
		owners.report(11, second, OwnerSource::Preferred, OwnerVerdict::Bad);
		choice = owners.choose(11, 0, fallback, self, true, 106);
		check(!choice.owner && !choice.lookup,
		      "exhausted reviewers wait for the lookup cooldown");
		check(owners.choose(11, 0, fallback, self, true, 100 + cooldown).lookup,
		      "the next lookup starts after the cooldown");
		owners.offer(11, {first});
		check(!owners.choose(11, 0, fallback, self, true, 100 + cooldown).owner,
		      "a failed reviewer stays excluded after a new lookup");
		check(!owners.choose(12, 0, 0, self, false, 0).lookup,
		      "MaxSchemaTries 0 switches the lookup off");
		check(!owners.choose(13, 0, self, self, false, 0).owner,
		      "the signed-in account is never its own owner");

		// Learned owners survive a restart: the next session's first request
		// (usually the library page) goes straight to them.
		Achievements::OwnerDirectory learned;
		learned.offer(20, {first, second});
		learned.report(20, first, OwnerSource::Candidate, OwnerVerdict::Bad);
		learned.offer(21, {first});
		learned.report(21, first, OwnerSource::Candidate, OwnerVerdict::Good);
		Achievements::OwnerDirectory restarted;
		restarted.restore(learned.serialize());
		choice = restarted.choose(20, 0, fallback, self, true, 0);
		check(choice.owner == second && choice.source == OwnerSource::Candidate,
		      "a restored game skips the fallback and its refused reviewer");
		choice = restarted.choose(21, 0, fallback, self, true, 0);
		check(choice.owner == first && choice.source == OwnerSource::Preferred,
		      "a restored game keeps the owner that answered");
		check(restarted.choose(22, 0, fallback, self, true, 0).source == OwnerSource::Fallback,
		      "a game without learned owners still starts with the fallback");
		Achievements::OwnerDirectory tolerant;
		tolerant.restore("junk\n23 0 12 76561198000000031\n24 x\n\n25 76561198000000032\n");
		check(tolerant.choose(23, 0, fallback, self, true, 0).owner == 76561198000000031ULL,
		      "restoring skips malformed ids");
		check(tolerant.choose(25, 0, fallback, self, true, 0).source == OwnerSource::Preferred,
		      "restoring reads a preferred owner without candidates");
		check(tolerant.choose(24, 0, fallback, self, true, 0).source == OwnerSource::Fallback,
		      "restoring ignores malformed lines");

		const std::string page =
			R"({"success":1,"reviews":[{"author":{"steamid":"76561198000000031"}},)"
			R"({"author":{"steamid":"76561198000000032"}},{"author":{"steamid":"76561198000000031"}},)"
			R"({"author":{"steamid":"12x"}},{"author":{"steamid":"76561198000000033"}}]})";
		check(Achievements::parseReviewers(page, 2) ==
		          std::vector<uint64_t>({76561198000000031ULL, 76561198000000032ULL}),
		      "reviewers parse in page order, deduplicated and capped");
		check(Achievements::parseReviewers(page, 10).size() == 3,
		      "malformed reviewer ids are skipped");
		check(Achievements::parseReviewers(R"({"steamid":"99999999999999999999999"})", 10).empty() &&
		      Achievements::parseReviewers(R"({"steamid":"123"})", 10).empty(),
		      "overflowing and non-individual ids are rejected");
		check(Achievements::parseReviewers("", 10).empty() &&
		      Achievements::parseReviewers(page, 0).empty(),
		      "an empty page or a zero count yields no reviewer");
		check(Achievements::reviewUrl(250900, 10) ==
		          "https://store.steampowered.com/appreviews/250900?json=1&filter=recent"
		          "&language=all&purchase_type=all&num_per_page=10",
		      "the review URL matches upstream's lookup");
	}
	std::vector<std::thread> threads;
	for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
		for (int n = 0; n < 1000; ++n) {
			auto c = store.context();
			store.observe(c.account, 123, true, 0, true, false, c.epoch);
			store.localEpoch(c.account, 123);
			if (n % 10 == 0) store.invalidate();
		}
	});
	for (auto& thread : threads) thread.join();
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
