// Regression tests for the native-CM batch wall-clock budget.
//
// Build (from repo root):
//   g++ -std=c++20 tools/test_cm_budget.cpp -o /tmp/t && /tmp/t

#include "../src/feats/cm_budget.hpp"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	using CmClient::remainingBudgetSeconds;

	CHECK(remainingBudgetSeconds(0, 15000, 30) == 15,
	      "first operation receives only the batch budget");
	CHECK(remainingBudgetSeconds(11000, 15000, 30) == 4,
	      "later operations receive only the remaining batch budget");
	CHECK(remainingBudgetSeconds(14999, 15000, 30) == 1,
	      "a positive sub-second remainder gets one final bounded attempt");
	CHECK(remainingBudgetSeconds(15000, 15000, 30) == 0,
	      "operation at the deadline is not started");
	CHECK(remainingBudgetSeconds(16000, 15000, 30) == 0,
	      "operation after the deadline is not started");
	CHECK(remainingBudgetSeconds(0, 15000, 6) == 6,
	      "per-operation cap remains below the whole batch budget");

	if (g_failures == 0) std::printf("\nall CM-budget checks passed\n");
	else std::printf("\n%d CM-budget check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
