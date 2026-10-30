// SPDX-License-Identifier: AGPL-3.0-only
//
// Wrapper integration: invokes a bundled helper script that
// processes Windows binaries belonging to AdditionalApps before
// Steam launches them through Proton. Drives Steamless under a
// dedicated Wine prefix; runs synchronously inside the
// IClientAppManager::LaunchApp hook.
//
// Layout expected at runtime:
//   <install-root>/tools/steamstub-bypass/run-steamless.sh
//   <install-root>/tools/steamless-bin/Steamless.CLI.exe
//   <install-root>/tools/steamless-bin/Plugins/*.dll
//
// The .so also probes ~/.local/share/SLSsteam/{steamstub-bypass,
// steamless-bin}/ as a user-local fallback for system-package
// installs where the install root is read-only.

#pragma once

#include <cstdint>

namespace SteamStub
{
	// Resolve helper paths from `installRoot` (the directory
	// holding SLSsteam.so).  Idempotent.  Logs a debug note and
	// disables the feature if the helper script or third-party
	// binaries are missing.
	void setup(const char* installRoot);

	// Called from the LaunchApp hook for every app launch.  No-ops
	// for apps not listed in AdditionalApps.  When the app's launch
	// options carry --steamless, scans the install dir for .exe files
	// and runs the helper on each one that needs processing (the first
	// one of a session after the helper prepared its Wine prefix),
	// blocking until the helper exits; otherwise restores the original
	// executables.  Re-entry safe via on-disk marker files.
	void onLaunchApp(uint32_t appId);
}
