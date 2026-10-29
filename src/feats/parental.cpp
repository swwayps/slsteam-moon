#include "parental.hpp"

#include "../config.hpp"
#include "../log.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace Parental
{
namespace
{

std::atomic<bool> g_warnedMalformed{ false };
std::shared_mutex g_lifecycleMutex;

using SettingsReceivedFn = bool(*)(void*, const uint8_t*, uint32_t,
	                               const uint8_t*, uint32_t,
	                               uintptr_t, uintptr_t);

lm_address_t g_receiver = LM_ADDRESS_BAD;
lm_address_t g_trampoline = LM_ADDRESS_BAD;
lm_size_t g_hookSize = 0;
lm_address_t g_signatureCheck = LM_ADDRESS_BAD;
uint8_t g_originalSignatureBranch = 0;
bool g_signaturePatched = false;

// Writes one code byte and puts the page protection back; false when either
// step failed. Callers read the byte to learn whether it changed.
bool writeByte(lm_address_t address, uint8_t value)
{
	lm_prot_t oldProt;
	if (!LM_ProtMemory(address, 1, LM_PROT_XRW, &oldProt))
	{
		return false;
	}
	const bool written = LM_WriteMemory(address, &value, 1) == 1;
	return LM_ProtMemory(address, 1, oldProt, LM_NULL) && written;
}

bool hkSettingsReceived(void* instance,
	                    const uint8_t* settings, uint32_t settingsLen,
	                    const uint8_t* signature, uint32_t signatureLen,
	                    uintptr_t a6, uintptr_t a7) noexcept
{
	try
	{
		std::shared_lock lifecycleLock(g_lifecycleMutex);
		// remove() frees the trampoline and leaves LM_ADDRESS_BAD for a call
		// that entered the detour before the unhook.
		if (g_trampoline == 0 || g_trampoline == LM_ADDRESS_BAD) return false;
		const auto original = reinterpret_cast<SettingsReceivedFn>(g_trampoline);
		if (!settings || settingsLen == 0)
		{
			return original(instance, settings, settingsLen, signature, signatureLen,
			                a6, a7);
		}

		auto rewritten = rewriteSettings(settings, settingsLen);
		if (!rewritten || rewritten->size() > std::numeric_limits<uint32_t>::max())
		{
			if (!g_warnedMalformed.exchange(true))
			{
				g_pLog->warn("Parental: received malformed settings; leaving them unchanged\n");
			}
			return original(instance, settings, settingsLen, signature, signatureLen,
			                a6, a7);
		}

		g_pLog->infoOnce("Parental: unlocked settings before Steam applied them (%u -> %zu bytes)\n",
		                 settingsLen, rewritten->size());
		return original(instance, rewritten->data(),
		                static_cast<uint32_t>(rewritten->size()),
		                signature, signatureLen, a6, a7);
	}
	catch (...)
	{
		try
		{
			if (!g_warnedMalformed.exchange(true))
			{
				g_pLog->warn("Parental: could not rewrite settings; leaving them unchanged\n");
			}
		}
		catch (...) {}

		// Do not read or call the trampoline without the lifecycle lock. A lock
		// acquisition failure is unrecoverable at this native callback boundary.
		return false;
	}
}

} // namespace

void setup()
{
	// Runs once, from Hooks::setup.
	std::unique_lock lifecycleLock(g_lifecycleMutex);
	if (!g_config.disableParentalRestrictions.get())
	{
		return;
	}

	const lm_address_t receiver = Patterns::ParentalSettingsReceived.address;
	if (receiver == 0 || receiver == LM_ADDRESS_BAD
	    || !looksLikeSettingsReceiver(reinterpret_cast<const uint8_t*>(receiver), 32))
	{
		g_pLog->warn("Parental: internal settings receiver unavailable; local unlock disabled\n");
		return;
	}

	const lm_address_t check = Patterns::ParentalSignatureCheck.address;
	if (check == 0 || check == LM_ADDRESS_BAD)
	{
		g_pLog->warn("Parental: signature check pattern unavailable; local unlock disabled\n");
		return;
	}

	const auto* bytes = reinterpret_cast<const uint8_t*>(check);
	if (bytes[0] != 0x84 || bytes[1] != 0xc0
	    || (bytes[2] != 0x75 && bytes[2] != 0xeb))
	{
		g_pLog->warn("Parental: signature check has unexpected instructions; local unlock disabled\n");
		return;
	}

	g_signatureCheck = check;
	g_originalSignatureBranch = bytes[2];
	if (bytes[2] == 0x75)
	{
		const bool patched = writeByte(check + 2, 0xeb);
		g_signaturePatched = bytes[2] == 0xeb;
		if (!patched)
		{
			g_pLog->warn("Parental: could not patch signature branch\n");
			return;
		}
	}

	g_receiver = receiver;
	g_hookSize = LM_HookCode(g_receiver,
	                        reinterpret_cast<lm_address_t>(&hkSettingsReceived),
	                        &g_trampoline);
	if (!g_hookSize || g_trampoline == 0 || g_trampoline == LM_ADDRESS_BAD)
	{
		g_hookSize = 0;
		g_trampoline = LM_ADDRESS_BAD;
		if (g_signaturePatched)
		{
			if (!writeByte(check + 2, g_originalSignatureBranch))
			{
				g_pLog->warn("Parental: could not restore signature branch after hook failure\n");
			}
			g_signaturePatched = bytes[2] == 0xeb;
		}
		g_pLog->warn("Parental: could not hook the internal settings receiver\n");
		return;
	}

	g_pLog->info("Parental: internal pre-cache restrictions unlock enabled\n");
}

void remove()
{
	std::unique_lock lifecycleLock(g_lifecycleMutex);
	if (g_hookSize)
	{
		if (!LM_UnhookCode(g_receiver, g_trampoline, g_hookSize))
		{
			g_pLog->warn("Parental: could not remove the internal settings hook\n");
			return;
		}
		g_hookSize = 0;
		g_trampoline = LM_ADDRESS_BAD;
	}
	if (g_signaturePatched)
	{
		if (writeByte(g_signatureCheck + 2, g_originalSignatureBranch))
		{
			g_signaturePatched = false;
		}
		else
		{
			g_pLog->warn("Parental: could not restore the signature branch\n");
		}
	}
}

} // namespace Parental
