#pragma once

// User-facing message catalog.
//
// Every desktop popup the end user can ever see is drawn from THIS catalog,
// as a friendly, localized sentence — never a raw developer diagnostic like
// "ManifestFetch: blob depot=445701 gid=... all CDN hosts failed (HTTP=503)".
// Those raw lines still go to ~/.SLSsteam.log for debugging; the screen only
// ever shows the catalog text.
//
// The catalog is PURE (no openssl / config / logger), like notify.hpp, so it
// is unit-testable in isolation (tools/test_usermsg.cpp).
//
// Adding a message:
//   1. add an enum value to UserMsg,
//   2. add EN + PT cases to messageFor(),
//   3. (the popup layer maps Severity -> timeout/urgency for you).
//
// Detail slot: a body may contain the literal "{detail}" sentinel; the popup
// layer splices a short runtime value (e.g. "HTTP 503") into it via
// substituteDetail(). Keep volatile data (codes, ids) OUT of the catalog and
// in the detail slot so the wording stays stable and translatable.

#include <string>

enum class Lang { English, Portuguese };
enum class Severity { Info, Warning, Error };

enum class UserMsg
{
	// --- lifecycle / info ---
	LoadSuccess,               // tool initialized OK
	BirthdayGreeting,          // easter egg (Feb 22) — replaces LoadSuccess

	// --- startup / compatibility (hard) ---
	SteamVersionUnsupported,   // unknown steamclient.so, SafeMode aborted load
	SteamVersionMismatch,      // hash mismatch, update recommended
	InitializationFailed,      // could not locate required patterns, aborted

	// --- install / download ---
	ContentServersUnavailable, // CDN unreachable / 5xx; {detail} = HTTP code
	DownloadAuthUnavailable,   // could not obtain a manifest request code
	DownloadTimedOut,          // timed out waiting on Steam's servers
	GamePreparationFailed,     // could not assemble a title's metadata
	DrmRemovalFailed,          // SteamStub unpack failed; title may not launch
	LocalStorageError,         // local write/extract failed (disk full / perms)

	// --- configuration ---
	ConfigUnreadable,          // config.yaml unreadable; using defaults
	ConfigParseFailed,         // config.yaml malformed; using defaults
	ConfigWriteFailed,         // could not create/save config
};

struct UiMessage
{
	const char* body;
	Severity    severity;
};

// Pick a language from an environment string of LANG / LC_MESSAGES /
// LC_ALL / LANGUAGE shape. "pt"/"pt_*" (incl. a colon list like "pt_BR:en")
// -> Portuguese; everything else, empty, or null -> English fallback.
inline Lang detectLang(const char* env)
{
	if (!env || env[0] == '\0') return Lang::English;
	// LANGUAGE may be a colon-separated priority list; the first entry wins.
	// LANG/LC_* are a single value. Either way we only look at the prefix.
	if ((env[0] == 'p' || env[0] == 'P') && (env[1] == 't' || env[1] == 'T'))
	{
		// Next char must be a separator or end so "pt"/"pt_BR"/"pt:.." match
		// but "ptb-something-else" style locales still resolve sanely (they
		// don't exist in practice; this just avoids matching e.g. "ptp").
		const char c = env[2];
		if (c == '\0' || c == '_' || c == '.' || c == ':' || c == '-' || c == ' ')
		{
			return Lang::Portuguese;
		}
	}
	return Lang::English;
}

// Replace the first "{detail}" sentinel in tmpl with `detail`. If `detail` is
// empty the sentinel (and a single adjacent space, if any) is removed so the
// sentence reads cleanly. Templates without the sentinel are returned as-is.
inline std::string substituteDetail(const std::string& tmpl, const std::string& detail)
{
	static const std::string kSentinel = "{detail}";
	const auto pos = tmpl.find(kSentinel);
	if (pos == std::string::npos) return tmpl;

	std::string out = tmpl;
	if (detail.empty())
	{
		size_t eraseFrom = pos;
		size_t eraseLen  = kSentinel.size();
		// Swallow one leading space so "code {detail}" -> "code".
		if (eraseFrom > 0 && out[eraseFrom - 1] == ' ')
		{
			eraseFrom -= 1;
			eraseLen  += 1;
		}
		out.erase(eraseFrom, eraseLen);
	}
	else
	{
		out.replace(pos, kSentinel.size(), detail);
	}
	return out;
}

// Localized "(and N more suppressed errors)" tail appended to a popup when the
// per-category throttle hid N repeats of the same message during its cooldown.
// N == 0 returns an empty string (caller appends nothing). Singular/plural is
// handled per language. This is the ONE bit of user-facing wording outside the
// catalog switch; keep it here so it stays translatable alongside everything else.
inline std::string suppressedSuffix(Lang lang, int n)
{
	if (n <= 0) return {};
	const std::string count = std::to_string(n);
	if (lang == Lang::Portuguese)
	{
		return n == 1 ? "(e mais " + count + " erro suprimido)"
		              : "(e mais " + count + " erros suprimidos)";
	}
	return n == 1 ? "(and " + count + " more suppressed error)"
	              : "(and " + count + " more suppressed errors)";
}

// The catalog. EN + PT for every UserMsg. First-draft wording — reviewed and
// tuned by hand (see .kiro/error-messages-review.md). Keep it: short, calm,
// plain-language, and actionable (tell the user what to do next).
inline UiMessage messageFor(UserMsg m, Lang lang)
{
	const bool pt = (lang == Lang::Portuguese);
	switch (m)
	{
		case UserMsg::LoadSuccess:
			return { pt ? "slsteam-moon carregou com sucesso."
			            : "slsteam-moon loaded successfully.",
			         Severity::Info };

		case UserMsg::BirthdayGreeting:
			return { pt ? "slsteam-moon carregou com sucesso. (E feliz aniversário, SLSsteam!)"
			            : "slsteam-moon loaded successfully. (And happy birthday, SLSsteam!)",
			         Severity::Info };

		case UserMsg::SteamVersionUnsupported:
			return { pt ? "O slsteam-moon foi desativado por segurança porque esta "
			              "versão do Steam não é compatível."
			            : "slsteam-moon was disabled for safety because this Steam "
			              "version isn't compatible.",
			         Severity::Error };

		case UserMsg::SteamVersionMismatch:
			return { pt ? "A Steam foi atualizada e ainda não validamos esta versão. "
			              "A maioria das coisas deve funcionar, mas se algo falhar, "
			              "procure uma atualização."
			            : "Steam was updated and we haven't validated this version yet. "
			              "Most things should still work; if something breaks, check "
			              "for an update.",
			         Severity::Warning };

		case UserMsg::InitializationFailed:
			return { pt ? "O slsteam-moon foi desativado por segurança. A Steam segue "
			              "inicializando sem modificações. (Padrões de código não "
			              "encontrados.)"
			            : "slsteam-moon was disabled for safety. Steam keeps starting "
			              "without modifications. (Code patterns not found.)",
			         Severity::Error };

		case UserMsg::ContentServersUnavailable:
			return { pt ? "Os servidores de conteúdo da Steam estão temporariamente "
			              "indisponíveis ({detail}). Aguarde alguns minutos e tente "
			              "baixar o jogo novamente."
			            : "Steam's content servers are temporarily unavailable "
			              "({detail}). Please wait a few minutes and try downloading "
			              "the game again.",
			         Severity::Error };

		case UserMsg::DownloadAuthUnavailable:
			return { pt ? "Não foi possível obter autorização de download junto à "
			              "Steam agora. Aguarde um momento e tente baixar o jogo "
			              "novamente."
			            : "Couldn't get download authorization from Steam right now. "
			              "Please wait a moment and try downloading the game again.",
			         Severity::Error };

		case UserMsg::DownloadTimedOut:
			return { pt ? "A Steam demorou demais para responder. Verifique sua "
			              "conexão e tente baixar o jogo novamente."
			            : "Steam took too long to respond. Check your connection and "
			              "try downloading the game again.",
			         Severity::Error };

		case UserMsg::GamePreparationFailed:
			return { pt ? "Não foi possível preparar os dados de um jogo. Reinicie o "
			              "Steam e tente instalar novamente."
			            : "Couldn't prepare a game's data. Please restart Steam and try "
			              "installing again.",
			         Severity::Error };

		case UserMsg::DrmRemovalFailed:
			return { pt ? "Não foi possível remover o DRM de um jogo, então ele pode "
			              "não abrir. Tente iniciá-lo de novo; se persistir, "
			              "reinstale-o."
			            : "Couldn't remove the game's DRM, so it may not launch. Try "
			              "starting it again; if it keeps failing, reinstall it.",
			         Severity::Error };

		case UserMsg::LocalStorageError:
			return { pt ? "Falha ao gravar arquivos no disco. Verifique se há espaço "
			              "livre e permissões na pasta da Steam."
			            : "Failed to write files to disk. Check that you have free "
			              "space and permissions in your Steam folder.",
			         Severity::Error };

		case UserMsg::ConfigUnreadable:
			return { pt ? "Não foi possível ler o arquivo de configuração; usando os "
			              "padrões por enquanto."
			            : "Couldn't read the configuration file; using defaults for now.",
			         Severity::Warning };

		case UserMsg::ConfigParseFailed:
			return { pt ? "O arquivo de configuração tem um erro de formatação; usando "
			              "os padrões por enquanto."
			            : "The configuration file has a formatting error; using "
			              "defaults for now.",
			         Severity::Warning };

		case UserMsg::ConfigWriteFailed:
			return { pt ? "Não foi possível salvar a configuração. Verifique as "
			              "permissões da sua pasta pessoal."
			            : "Couldn't save the configuration. Check the permissions on "
			              "your home folder.",
			         Severity::Error };
	}
	// Unreachable for a valid enum; keep the compiler happy and fail safe.
	return { pt ? "Ocorreu um erro inesperado."
	            : "An unexpected error occurred.",
	         Severity::Error };
}
