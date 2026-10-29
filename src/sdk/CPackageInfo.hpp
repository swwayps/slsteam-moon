// SPDX-License-Identifier: AGPL-3.0-only
//
// In-process Steam structures for the LoadPackage hook.
//
// Mirrors LumaCore's struct definitions. The i386 offsets were verified on
// Linux Steam build b9052d72350d38af101463fb2b5334b8 (CPackageInfo's
// constructor, LoadPackage body and BUpdateLicenses subscriber loop). The
// x86-64 offsets below are the natural layout of the same fields and are NOT
// verified yet: LoadPackage has no x86-64 locator, so nothing reads them.

#pragma once

#include "CUtl.hpp"
#include "types.hpp"

#include <cstdint>


enum class EPackageStatus : uint32_t
{
	Available = 0,
	Preorder = 1,
	Unavailable = 2,
	Invalid = 3
};

enum class EBillingType : uint32_t {};

static_assert(sizeof(CUtlMemory<uint32_t>) == 0x10, "CUtlMemory layout drift");
static_assert(sizeof(CUtlVector<uint32_t>) == 0x18, "CUtlVector layout drift");

struct PackageInfo
{
	uint32_t PackageId;        // +0x00
	int32_t ChangeNumber;      // +0x04
	uint64_t PICS_token;       // +0x08
	EBillingType BillingType;  // +0x10
	ELicenseType LicenseType;  // +0x14
	EPackageStatus Status;     // +0x18
	uint8_t SHA_1_Hash[20];    // +0x1C
	void* pPackageInfoNodeBegin; // +0x30
	void* pExtendNodeBegin;    // +0x38
	CUtlVector<uint32_t> AppIdVec;   // +0x40..+0x57
	CUtlVector<uint32_t> DepotIdVec; // +0x58..+0x6F
	// runtime cache tracking follows — do not touch
};
static_assert(offsetof(PackageInfo, Status)     == 0x18, "PackageInfo::Status drift");
static_assert(offsetof(PackageInfo, SHA_1_Hash) == 0x1C, "PackageInfo::SHA_1_Hash drift");
static_assert(offsetof(PackageInfo, AppIdVec)   == 0x40, "PackageInfo::AppIdVec drift");
static_assert(offsetof(PackageInfo, DepotIdVec) == 0x58, "PackageInfo::DepotIdVec drift");
