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
