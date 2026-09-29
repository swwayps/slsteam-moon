#pragma once

#include <cstdint>
#include <string>



typedef uint32_t AppId_t;
typedef uint64_t GameId_t;

typedef int32_t ENetPacket;

typedef uint32_t HSteamPipe;
typedef uint32_t HSteamUser;


constexpr uint64_t GAME_TYPE_SHORTCUT = 0x2000000ULL;

constexpr static ENetPacket INVALID_NETPACKET_TYPE = -1;
constexpr static ENetPacket PROTOBUF_TYPE_MASK = 0x80000000;

enum EResult
{
	k_EResultNoResult = 0x0,
	k_EResultOK = 0x1,
	k_EResultFailure = 0x2,
	k_EResultNoConnection = 0x3,
	k_EResultUnknown = 0x4,
	k_EResultInvalidPassword = 0x5,
	k_EResultLoggedInElsewhere = 0x6,
	k_EResultInvalidProtocol = 0x7,
	k_EResultInvalidParameter = 0x8,
	k_EResultFileNotFound = 0x9,
	k_EResultBusy = 0xa,
	k_EResultInvalidState = 0xb,
	k_EResultInvalidName = 0xc,
	k_EResultInvalidEmail = 0xd,
	k_EResultDuplicateName = 0xe,
	k_EResultAccessDenied = 0xf,
	k_EResultTimeout = 0x10,
	k_EResultBanned = 0x11,
	k_EResultAccountNotFound = 0x12,
	k_EResultInvalidSteamID = 0x13,
	k_EResultServiceUnavailable = 0x14,
	k_EResultNotLoggedOn = 0x15,
	k_EResultPending = 0x16,
	k_EResultEncryptionFailure = 0x17,
	k_EResultInsufficientPrivilege = 0x18,
	k_EResultLimitexceeded = 0x19,
	k_EResultRequestrevoked = 0x1a,
	k_EResultLicenseexpired = 0x1b,
	k_EResultAlreadyRedeemed = 0x1c,
	k_EResultDuplicatedRequest = 0x1d,
	k_EResultAlreadyOwned = 0x1e,
	k_EResultIPAddressNotFound = 0x1f,
	k_EResultPersistenceFailed = 0x20,
	k_EResultLockingFailed = 0x21,
	k_EResultSessionReplaced = 0x22,
	k_EResultConnectionFailed = 0x23,
	k_EResultHandshakeFailed = 0x24,
	k_EResultIOOperationFailed = 0x25,
	k_EResultDisconnectedByRemoteHost = 0x26,
	k_EResultShoppingCartNotFound = 0x27,
	k_EResultBlocked = 0x28,
	k_EResultIgnored = 0x29,
	k_EResultNomatch = 0x2a,
	k_EResultAccountDisabled = 0x2b,
	k_EResultServiceReadOnly = 0x2c,
	k_EResultAccountNotFeatured = 0x2d,
	k_EResultAdministratorOK = 0x2e,
	k_EResultContentVersion = 0x2f,
	k_EResultTryanotherCM = 0x30,
	k_EResultPasswordrequiredtokicksession = 0x31,
	k_EResultAlreadyLoggedInElsewhere = 0x32,
	k_EResultRequestsuspendedOrpaused = 0x33,
	k_EResultRequesthasbeencanceled = 0x34,
	k_EResultCorruptedorunrecoverabledataerror = 0x35,
	k_EResultNotenoughfreediskspace = 0x36,
	k_EResultRemotecallfailed = 0x37,
	k_EResultPasswordisnotset = 0x38,
	k_EResultExternalAccountisnotlinkedtoaSteamaccount = 0x39,
	k_EResultPSNTicketisinvalid = 0x3a,
	k_EResultExternalAccountlinkedtoanotherSteamaccount = 0x3b,
	k_EResultRemoteFileConflict = 0x3c,
	k_EResultIllegalpassword = 0x3d,
	k_EResultSameaspreviousvalue = 0x3e,
	k_EResultAccountLogonDenied = 0x3f,
	k_EResultCannotUseOldPassword = 0x40,
	k_EResultInvalidLoginAuthCode = 0x41,
	k_EResultAccountLogonDeniednomailsent = 0x42,
	k_EResultHardwarenotcapableofIPT = 0x43,
	k_EResultIPTiniterror = 0x44,
	k_EResultOperationfailedduetoparentalcontrolrestrictionsforcurrentuser = 0x45,
	k_EResultFacebookqueryreturnedanerror = 0x46,
	k_EResultExpiredLoginAuthCode = 0x47,
	k_EResultIPLoginRestrictionFailed = 0x48,
	k_EResultAccountLockedDown = 0x49,
	k_EResultAccountLogonDeniedVerifiedEmailRequired = 0x4a,
	k_EResultNomatchingURL = 0x4b,
	k_EResultBadresponse = 0x4c,
	k_EResultPasswordreentryrequired = 0x4d,
	k_EResultValueisoutofrange = 0x4e,
	k_EResultUnexpectederror = 0x4f,
	k_EResultFeatureDisabled = 0x50,
	k_EResultInvalidCEGSubmission = 0x51,
	k_EResultRestricteddevice = 0x52,
	k_EResultRegionLocked = 0x53,
	k_EResultRateLimitExceeded = 0x54,
	k_EResultAccountlogondeniedneedtwofactorcode = 0x55,
	k_EResultItemorentryhasbeendeleted = 0x56,
	k_EResultToomanylogonattempts = 0x57,
	k_EResultTwofactorcodemismatch = 0x58,
	k_EResultTwofactoractivationcodemismatch = 0x59,
	k_EResultAccountassociatedwithmultipleplayers = 0x5a,
	k_EResultNotModified = 0x5b,
	k_EResultNomobiledeviceavailable = 0x5c,
	k_EResultTimeisoutofsync = 0x5d,
	k_EResultSMScodefailed = 0x5e,
	k_EResultToomanyaccountsaccessthisresource = 0x5f,
	k_EResultToomanychangestothisaccount = 0x60,
	k_EResultToomanychangestothisphonenumber = 0x61,
	k_EResultYoumustrefundthistransactiontowallet = 0x62,
	k_EResultSendingofanemailfailed = 0x63,
	k_EResultPurchasenotyetsettled = 0x64,
	k_EResultNeedscaptcha = 0x65,
	k_EResultGameserverlogintokendenied = 0x66,
	k_EResultGameserverlogintokenownerdenied = 0x67,
	k_EResultInvaliditemtype = 0x68,
	k_EResultIPAddressBanned = 0x69,
	k_EResultGameserverlogintokenexpired = 0x6a,
	k_EResultInsufficientfunds = 0x6b,
	k_EResultToomanypending = 0x6c,
	k_EResultNositelicensesfound = 0x6d,
	k_EResultNetworksendexceeded = 0x6e,
	k_EResultAccountsnotfriends = 0x6f,
	k_EResultLimiteduseraccount = 0x70,
	k_EResultCantremoveitem = 0x71,
	k_EResultAccounthasbeendeleted = 0x72,
	k_EResultAccounthasanexistingusercancelledlicense = 0x73,
	k_EResultDeniedduetocommunitycooldown = 0x74,
	k_EResultNolauncherspecified = 0x75,
	k_EResultMustagreetoSSA = 0x76,
	k_EResultClientnolongersupported = 0x77,
	k_EResultThecurrentSteamrealmdoesnotmatchtherequestedresource = 0x78,
	k_EResultSignaturecheckfailed = 0x79,
	k_EResultFailedtoparseinput = 0x7a,
	k_EResultNoverifiedphonenumber = 0x7b,
	k_EResultInsufficientbatterycharge = 0x7c,
	k_EResultChargerrequired = 0x7d,
	k_EResultCachedcredentialisinvalid = 0x7e,
	k_EResultPhonenumberisVoiceOverIP = 0x7f,
	k_EResultThedatabeingaccessedisnotsupportedbythisAPI = 0x80,
	k_EResultMaximumfamilysizeexceeded = 0x81,
	k_EResultOfflineAppCacheinvalid = 0x82,
	k_EResultRetrylater = 0x83,
};

enum class EIPCCmd : uint8_t
{
	RunInterface = 1,
	SerializeCallbacks = 2,
	CreateGlobalUser = 3,
	DisconnectGlobalUser = 4,
	ClosePipe = 5,
	Heartbeat = 6,
	ConnectPipe = 9
};

std::string EIPCCmd_ToString(const EIPCCmd cmd);

enum class EIPCExitCode : uint8_t
{
	Success = 0xb
};

enum class EIPCInterface : uint8_t
{
	User = 0x1,
	GameServerInternal = 0x2,
	Friends = 0x3,
	Utils = 0x4,
	Billing = 0x5,
	Matchmaking = 0x6,
	Apps = 0x8,
	UserStats = 0xb,
	Networking = 0xc,
	RemoteStorage = 0xd,
	DepotBuilder = 0x10,
	AppManager = 0x11,
	ConfigStore = 0x12,
	GameCoordinator = 0x13,
	GameServerStats = 0x14,
	GameStats = 0x15,
	HTTP = 0x16,
	Screenshots = 0x17,
	Audio = 0x18,
	UnifiedMessages = 0x19,
	StreamLauncher = 0x1a,
	ParentalSettings = 0x1b,
	NetworkDeviceManager = 0x1d,
	Music = 0x1e,
	RemoteClientManager = 0x1f,
	UGC = 0x20,
	StreamClient = 0x21,
	ProductBuilder = 0x22,
	Shortcuts = 0x23,
	GameNotifications = 0x25,
	Video = 0x26,
	Inventory = 0x27,
	VR = 0x28,
	ControllerSerialized = 0x29,
	AppDisableUpdate = 0x2a,
	SharedConnection = 0x2c,
	Shader = 0x2d,
	NetworkingSocketsSerialized = 0x2e,
	Compat = 0x30,
	Parties = 0x31,
	NetworkingUtilsSerialized = 0x32,
	RemotePlay = 0x34,
	GameServerPacketHandler = 0x35,
	SystemManager = 0x36,
	SystemPerfManager = 0x39,
	SystemDockManager = 0x3a,
	SystemAudioManager = 0x3b,
	SystemDisplayManager = 0x3c,
	Timeline = 0x3d
};

std::string EIPCInterface_ToString(const EIPCInterface interface);

namespace Steam
{
	typedef void*(*Plat_Alloc_t)(int);
	typedef void(*Plat_Free_t)(void*);
	typedef void*(*Plat_Realloc_t)(void*, int);

	extern Plat_Alloc_t Plat_Alloc;
	extern Plat_Free_t Plat_Free;
	extern Plat_Realloc_t Plat_Realloc;

	bool init();
}
