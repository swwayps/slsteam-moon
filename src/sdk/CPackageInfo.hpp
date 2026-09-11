// SPDX-License-Identifier: AGPL-3.0-only
//
// In-process Steam structures for the LoadPackage hook.
//
// Mirrors LumaCore's struct definitions but with Linux i386 (4-byte
// pointer) layout. Field offsets verified against the Linux Steam build
// b9052d72350d38af101463fb2b5334b8 by reverse engineering CPackageInfo's
// constructor, LoadPackage body and BUpdateLicenses subscriber loop.
//
// The first 0x58 bytes of PackageInfo match LumaCore's Windows layout;
// the trailing bytes (0x58..0x77) are cache tracking and not
// touched by us.

#pragma once

#include "CUtl.hpp"

#include <cstdint>


enum class EPackageStatus : uint32_t
{
	Available = 0,
	Preorder = 1,
	Unavailable = 2,
	Invalid = 3
};

enum class EBillingType : uint32_t {};
enum class ELicenseType : uint32_t {};

static_assert(sizeof(CUtlMemory<uint32_t>) == 12, "CUtlMemory layout drift");

static_assert(sizeof(CUtlVector<uint32_t>) == 16, "CUtlVector layout drift");

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
	void* pExtendNodeBegin;    // +0x34
	CUtlVector<uint32_t> AppIdVec;   // +0x38..+0x47
	CUtlVector<uint32_t> DepotIdVec; // +0x48..+0x57
	// +0x58..+0x77 runtime cache tracking — do not touch
};
static_assert(offsetof(PackageInfo, Status)     == 0x18, "PackageInfo::Status drift");
static_assert(offsetof(PackageInfo, SHA_1_Hash) == 0x1C, "PackageInfo::SHA_1_Hash drift");
static_assert(offsetof(PackageInfo, AppIdVec)   == 0x38, "PackageInfo::AppIdVec drift");
static_assert(offsetof(PackageInfo, DepotIdVec) == 0x48, "PackageInfo::DepotIdVec drift");
