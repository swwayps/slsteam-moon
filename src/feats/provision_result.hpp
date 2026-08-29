#pragma once

namespace AppInfoProvision
{

enum class SourceResult
{
	Success,
	InvalidResponse,
	IncompleteContent,
	NoUsableContent,
	VirtualDlc,
	LocalFailure,
};

enum class ProvisionOutcome
{
	Updated,
	FreshCache,
	FallbackCache,
	NetworkUnavailable,
	IncompleteContent,
	NoUsableContent,
	NotApplicable,
	LocalFailure,
};

enum class ProvisionNotice
{
	None,
	MetadataUnavailable,
	ReviewGameData,
	LocalStorage,
};

inline bool isProvisioned(ProvisionOutcome outcome)
{
	return outcome == ProvisionOutcome::Updated
	    || outcome == ProvisionOutcome::FreshCache
	    || outcome == ProvisionOutcome::FallbackCache;
}

inline bool isTerminalOutcome(ProvisionOutcome outcome)
{
	return outcome == ProvisionOutcome::NotApplicable ||
	       outcome == ProvisionOutcome::NoUsableContent;
}

// Reloading Steam's live appinfo map is a recovery action for a newly added
// app or a response we deliberately suppressed. The guarded injection path
// validates the pair again, so a fresh/stale local pair and an offline fallback
// are equally valid readiness inputs.
inline bool runtimePublicationAllowed(bool requested,
                                      ProvisionOutcome outcome) noexcept
{
	return requested && isProvisioned(outcome);
}

inline ProvisionNotice noticeForOutcome(ProvisionOutcome outcome)
{
	switch (outcome)
	{
		case ProvisionOutcome::Updated:
		case ProvisionOutcome::FreshCache:
		case ProvisionOutcome::FallbackCache:
		case ProvisionOutcome::NotApplicable:
			return ProvisionNotice::None;
		case ProvisionOutcome::NetworkUnavailable:
			return ProvisionNotice::MetadataUnavailable;
		case ProvisionOutcome::IncompleteContent:
		case ProvisionOutcome::NoUsableContent:
			return ProvisionNotice::ReviewGameData;
		case ProvisionOutcome::LocalFailure:
			return ProvisionNotice::LocalStorage;
	}
	return ProvisionNotice::ReviewGameData;
}

inline SourceResult classifyContentResult(bool hadConcreteContent,
                                          bool hasUsableContent)
{
	if (hasUsableContent) return SourceResult::Success;
	return hadConcreteContent
	    ? SourceResult::NoUsableContent
	    : SourceResult::IncompleteContent;
}

// A product-info record whose common.type is DLC may legitimately contain
// only virtual ownership metadata.  It is not incomplete game content and
// must not trigger a provider retry or a user-facing preparation warning.
inline SourceResult classifyContentResult(bool hadConcreteContent,
                                          bool hasUsableContent,
                                          bool isDlc)
{
	if (isDlc && !hadConcreteContent && !hasUsableContent)
		return SourceResult::VirtualDlc;
	return classifyContentResult(hadConcreteContent, hasUsableContent);
}

inline bool shouldTryProviderFallback(SourceResult result)
{
	return result == SourceResult::InvalidResponse
	    || result == SourceResult::IncompleteContent;
}

} // namespace AppInfoProvision
