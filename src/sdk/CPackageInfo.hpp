// SPDX-License-Identifier: AGPL-3.0-only
//
// In-process Steam structures for the LoadPackage hook.
//
// Mirrors LumaCore's struct definitions. x86-64 offsets verified on the
// steamrt64 beta (steamclient.so build-id 13f56b24...): ReadFromDisk allocates
// 0xa0 bytes; the constructor (0x1052b10) sets Status=3 at +0x18, zeroes the
// SHA-1 at +0x1C..+0x2F, constructs the CUtlMemory of AppIdVec at +0x40 and of
// DepotIdVec at +0x58 and zeroes their sizes at +0x50/+0x68; LoadPackage
// grows both through CUtlMemory::Grow(&vec, n).

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
	// +0x70..+0x9F runtime cache tracking — do not touch
};
static_assert(offsetof(PackageInfo, Status)     == 0x18, "PackageInfo::Status drift");
static_assert(offsetof(PackageInfo, SHA_1_Hash) == 0x1C, "PackageInfo::SHA_1_Hash drift");
static_assert(offsetof(PackageInfo, AppIdVec)   == 0x40, "PackageInfo::AppIdVec drift");
static_assert(offsetof(PackageInfo, DepotIdVec) == 0x58, "PackageInfo::DepotIdVec drift");
