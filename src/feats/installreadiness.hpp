// SPDX-License-Identifier: AGPL-3.0-only
//
// InstallReadiness publishes a short-lived, per-AppID decision for Lumen's
// synchronous Install Wizard guard. It never decides whether Steam owns an app
// and never touches the UI; it only states whether a managed app is known to
// have no safe manifest path while request-code providers are cooling down.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace InstallReadiness
{
	struct Observation
	{
		std::uint32_t appId = 0;
		std::size_t targetCount = 0;
		std::size_t localCount = 0;
		bool exactPins = false;
	};

	inline bool shouldBlock(bool providersOffline, const Observation& app)
	{
		if (!providersOffline || !app.appId || app.targetCount == 0) return false;
		return app.exactPins ? app.localCount < app.targetCount
		                     : app.localCount == 0;
	}

	inline std::string serialize(std::int64_t updatedAt,
	                             std::vector<Observation> observations,
	                             bool providersOffline)
	{
		std::sort(observations.begin(), observations.end(),
		          [](const Observation& left, const Observation& right)
		          { return left.appId < right.appId; });
		std::string out = "{\"version\":1,\"updated_at\":"
		    + std::to_string(updatedAt) + ",\"apps\":{";
		bool first = true;
		for (const auto& app : observations)
		{
			if (!app.appId) continue;
			if (!first) out += ',';
			first = false;
			out += '\"' + std::to_string(app.appId) + "\":{";
			out += "\"blocked\":";
			out += shouldBlock(providersOffline, app) ? "true" : "false";
			out += ",\"targets\":" + std::to_string(app.targetCount);
			out += ",\"local\":" + std::to_string(app.localCount);
			out += ",\"exact_pins\":";
			out += app.exactPins ? "true" : "false";
			out += '}';
		}
		out += "}}\n";
		return out;
	}

	// Atomic heartbeat for the sidecar. Failure is deliberately silent to the
	// UI: Lumen treats a missing/stale snapshot as fail-open.
	bool publish(const std::vector<Observation>& observations,
	             bool providersOffline);
}
