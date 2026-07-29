#pragma once

namespace AppInfoProvision
{

enum class SourceResult
{
	Success,
	InvalidResponse,
	IncompleteContent,
	NoUsableContent,
	LocalFailure,
};

enum class ProvisionOutcome
{
	Updated,
	FreshCache,
	FallbackCache,
	NetworkUnavailable,
	IncompleteContent,
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

inline ProvisionNotice noticeForOutcome(ProvisionOutcome outcome)
{
	switch (outcome)
	{
		case ProvisionOutcome::Updated:
		case ProvisionOutcome::FreshCache:
		case ProvisionOutcome::FallbackCache:
			return ProvisionNotice::None;
		case ProvisionOutcome::NetworkUnavailable:
			return ProvisionNotice::MetadataUnavailable;
		case ProvisionOutcome::IncompleteContent:
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

inline bool shouldTryProviderFallback(SourceResult result)
{
	return result == SourceResult::InvalidResponse
	    || result == SourceResult::IncompleteContent;
}

} // namespace AppInfoProvision
