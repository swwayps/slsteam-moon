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

struct ByteWriteResult
{
	bool valueWritten;
	bool protectionRestored;

	explicit operator bool() const
	{
		return valueWritten && protectionRestored;
	}
};

ByteWriteResult writeByte(lm_address_t address, uint8_t value)
{
	lm_prot_t oldProt;
	if (!LM_ProtMemory(address, 1, LM_PROT_XRW, &oldProt))
	{
		return { false, false };
	}
	const lm_size_t written = LM_WriteMemory(address, &value, 1);
	const bool valueWritten = written == 1
	                       && *reinterpret_cast<const uint8_t*>(address) == value;
	bool restored = LM_ProtMemory(address, 1, oldProt, LM_NULL);
	if (!restored)
	{
		restored = LM_ProtMemory(address, 1, oldProt, LM_NULL);
	}
	return { valueWritten, restored };
}

bool hkSettingsReceived(void* instance,
	                    const uint8_t* settings, uint32_t settingsLen,
	                    const uint8_t* signature, uint32_t signatureLen,
	                    uintptr_t a6, uintptr_t a7) noexcept
{
	try
	{
		std::shared_lock lifecycleLock(g_lifecycleMutex);
		const auto original = reinterpret_cast<SettingsReceivedFn>(g_trampoline);
		if (!original) return false;
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
	std::unique_lock lifecycleLock(g_lifecycleMutex);
	g_warnedMalformed = false;
	if (!g_config.disableParentalRestrictions.get())
	{
		return;
	}
	if (g_hookSize)
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

	if (g_signaturePatched)
	{
		// A previous teardown/hook rollback could not restore the byte. Keep the
		// original value already recorded instead of replacing it with 0xeb.
		if (g_signatureCheck != check || bytes[2] != 0xeb)
		{
			g_pLog->warn("Parental: pending signature restoration is inconsistent\n");
			return;
		}
	}
	else
	{
		g_signatureCheck = check;
		g_originalSignatureBranch = bytes[2];
		if (bytes[2] == 0x75)
		{
			const auto patched = writeByte(check + 2, 0xeb);
			g_signaturePatched = patched.valueWritten;
			if (!patched)
			{
				g_pLog->warn("Parental: could not patch signature branch\n");
				return;
			}
		}
	}

	g_receiver = receiver;
	g_hookSize = LM_HookCode(g_receiver,
	                        reinterpret_cast<lm_address_t>(&hkSettingsReceived),
	                        &g_trampoline);
	if (!g_hookSize || g_trampoline == 0 || g_trampoline == LM_ADDRESS_BAD)
	{
		if (g_signaturePatched)
		{
			const auto restored =
				writeByte(g_signatureCheck + 2, g_originalSignatureBranch);
			if (restored.valueWritten)
			{
				g_signaturePatched = false;
			}
			if (!restored)
			{
				g_pLog->warn("Parental: could not restore signature branch after hook failure\n");
			}
		}
		g_receiver = LM_ADDRESS_BAD;
		g_trampoline = LM_ADDRESS_BAD;
		g_pLog->warn("Parental: could not hook the internal settings receiver\n");
		return;
	}

	g_pLog->info("Parental: internal pre-cache restrictions unlock enabled\n");
}

void remove()
{
	std::unique_lock lifecycleLock(g_lifecycleMutex);
	if (g_hookSize && g_receiver != LM_ADDRESS_BAD && g_trampoline != LM_ADDRESS_BAD)
	{
		if (!LM_UnhookCode(g_receiver, g_trampoline, g_hookSize))
		{
			g_pLog->warn("Parental: could not remove the internal settings hook\n");
			return;
		}
	}
	g_hookSize = 0;
	g_receiver = LM_ADDRESS_BAD;
	g_trampoline = LM_ADDRESS_BAD;

	if (g_signaturePatched && g_signatureCheck != LM_ADDRESS_BAD)
	{
		const auto restored =
			writeByte(g_signatureCheck + 2, g_originalSignatureBranch);
		if (restored.valueWritten)
		{
			g_signaturePatched = false;
		}
		if (!restored)
		{
			g_pLog->warn("Parental: could not restore the signature branch\n");
			return;
		}
	}
	g_signaturePatched = false;
	g_signatureCheck = LM_ADDRESS_BAD;
}

} // namespace Parental
