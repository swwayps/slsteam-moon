// SPDX-License-Identifier: AGPL-3.0-only
//
// Background work for the achievements schema owner: reviewer lookups and
// saving what was learned. Steam's threads only queue here; one detached
// worker does the HTTP request, parsing and file write.

#include "achievements.hpp"
#include "../curl.hpp"
#include "../log.hpp"
#include "../thread_start.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
// app == 0 is a save of the learned owners.
struct Job
{
	uint32_t app = 0;
	uint32_t count = 0;
};

std::mutex g_mutex;
std::vector<Job> g_queue;
bool g_active = false;

void runLookup(const Job& job)
{
	std::vector<uint64_t> found;
	bool fetched = false;
	try
	{
		std::string page;
		fetched = Curl::downloadString(
			Achievements::reviewUrl(job.app, job.count).c_str(), page) == 0;
		if (fetched)
			found = Achievements::parseReviewers(page, job.count);
	}
	catch (...)
	{
		fetched = false;
	}
	// Always answer, so the directory stops waiting for this lookup.
	Achievements::offerOwners(job.app, found);
	g_pLog->info("Achievements: reviewer lookup for app=%u %s %zu owner(s)\n",
	             job.app, fetched ? "found" : "failed;", found.size());
}

void runJobs()
{
	for (;;)
	{
		Job job;
		{
			std::lock_guard lock(g_mutex);
			if (g_queue.empty())
			{
				g_active = false;
				return;
			}
			job = g_queue.front();
			g_queue.erase(g_queue.begin());
		}
		if (job.app)
			runLookup(job);
		else
			Achievements::saveOwners();
	}
}

// Worker start or body failed: release every queued lookup.
void abandonQueued()
{
	std::vector<Job> dropped;
	{
		std::lock_guard lock(g_mutex);
		dropped.swap(g_queue);
		g_active = false;
	}
	for (const Job& job : dropped)
	{
		if (job.app) Achievements::offerOwners(job.app, {});
	}
}

void enqueue(Job job)
{
	{
		std::lock_guard lock(g_mutex);
		if (std::none_of(g_queue.begin(), g_queue.end(),
		                 [&job](const Job& queued) { return queued.app == job.app; }))
			g_queue.push_back(job);
		if (g_active) return;
		g_active = true;
	}
	ThreadStart::startDetached(
		[]
		{
			ThreadStart::runGuarded(runJobs, []
			{
				if (g_pLog)
					g_pLog->warn("Achievements: owner lookup worker failed; "
					             "queued lookups are answered empty\n");
				abandonQueued();
			}, [] {});
		},
		[]
		{
			if (g_pLog)
				g_pLog->warn("Achievements: owner lookup worker did not start; "
				             "queued lookups are answered empty\n");
			abandonQueued();
		});
}
}

void Achievements::requestOwnerLookup(uint32_t app, uint32_t count)
{
	if (app) enqueue({app, count});
}

void Achievements::requestOwnerSave()
{
	enqueue({0, 0});
}
