// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>

namespace CmClient
{

// Return the whole-second curl timeout available to the next CM operation.
// A positive fractional remainder is rounded up so curl receives one final
// bounded attempt; callers still enforce the exact steady-clock deadline.
inline long remainingBudgetSeconds(long long elapsedMs, long long budgetMs,
                                   long operationCapSecs)
{
	if (budgetMs <= 0 || operationCapSecs <= 0 || elapsedMs >= budgetMs)
		return 0;
	const long long remainingMs = budgetMs - std::max(0LL, elapsedMs);
	const long long roundedSecs = (remainingMs + 999) / 1000;
	return static_cast<long>(std::min<long long>(roundedSecs, operationCapSecs));
}

} // namespace CmClient
