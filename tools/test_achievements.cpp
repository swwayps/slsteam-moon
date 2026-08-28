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
	check(!store.localEpoch(8, 123), "an observation cannot cross accounts");
	store.observe(7, 123, true, 25, true, false, context.epoch);
	store.observe(7, 123, true, 0, true, false, context.epoch);
	check(!store.localEpoch(7, 123), "a real package wins over package zero until license invalidation");
	store.invalidate();
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
	check(Achievements::resolveOwnerSteamId(1, {{1, 42}}, 21) == 42 &&
	      Achievements::resolveOwnerSteamId(1, {{1, 0}}, 21) == 21,
	      "per-app owner override retains its fallback rule");
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
