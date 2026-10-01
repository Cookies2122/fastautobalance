#include "fastautobalance.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

FastAutoBalance g_FastAutoBalance;
PLUGIN_EXPOSE(FastAutoBalance, g_FastAutoBalance);

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;
IVIPApi* g_pVIPCore = nullptr;
IAdminApi* g_pAdminCore = nullptr;
IVEngineServer2* engine = nullptr;
IFileSystem* filesystem = nullptr;
ISource2GameClients* g_pGameClients = nullptr;

CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars* gpGlobals = nullptr;

SH_DECL_HOOK2_void(ISource2GameClients, ClientCommand, SH_NOATTRIB, 0, CPlayerSlot, const CCommand&);

struct BalanceInfo
{
	int iNewTeam;
	int iOldTeam;
	int iRound;
};

int g_iMaxDiff;
int g_iBlockDiff;
bool g_bMessage;
bool g_bDebug;
bool g_bForceNativeOff;

bool g_bAdminImmunity;
std::string g_sAdminFlag;
int g_iAdminMaxDiff;
int g_iAdminBlockDiff;

bool g_bVipImmunity;
std::vector<std::string> g_vecVipGroups;
int g_iVipMaxDiff;
int g_iVipBlockDiff;

int g_iTeam[64];
bool g_bAlive[64];
bool g_bPluginMove[64];
time_t g_iBlockMsgTime[64];
int g_iBlockMsgSkip[64];

std::map<int, BalanceInfo> g_Queue;
int g_iRound = 0;
bool g_bPrestart = false;
bool g_bLateLoad = false;
CTimer* g_pCheckTimer = nullptr;

std::map<std::string, std::string> g_vecPhrases;

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils ? g_pUtils->GetCGameEntitySystem() : nullptr;
}

const char* GetTeamName(int iTeam)
{
	switch (iTeam)
	{
		case 1: return "SPEC";
		case 2: return "T";
		case 3: return "CT";
	}
	return "NONE";
}

bool IsValidPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64 || !g_pPlayers)
		return false;
	return g_pPlayers->IsConnected(iSlot) && !g_pPlayers->IsFakeClient(iSlot);
}

const char* GetClientName(int iSlot)
{
	const char* szName = g_pPlayers ? g_pPlayers->GetPlayerName(iSlot) : nullptr;
	return (szName && szName[0]) ? szName : "Unknown";
}

void ResetPlayer(int iSlot)
{
	g_iTeam[iSlot] = 0;
	g_bAlive[iSlot] = false;
	g_bPluginMove[iSlot] = false;
	g_iBlockMsgTime[iSlot] = 0;
	g_iBlockMsgSkip[iSlot] = 0;
}

bool CreateDir(const char* szPath)
{
#ifdef _WIN32
	return _mkdir(szPath) == 0 || errno == EEXIST;
#else
	return mkdir(szPath, 0755) == 0 || errno == EEXIST;
#endif
}

void LogDebug(const char* szFormat, ...)
{
	if (!g_bDebug)
		return;

	static bool bError = false;
	char szPath[512];

	const char* szDirs[] = { "addons", "addons/logs", "addons/logs/fab" };
	for (int i = 0; i < 3; i++)
	{
		g_SMAPI->PathFormat(szPath, sizeof(szPath), "%s/%s", g_SMAPI->GetBaseDir(), szDirs[i]);
		if (!CreateDir(szPath))
		{
			if (!bError)
			{
				bError = true;
				Msg("[FAB] Failed to create %s (errno %d)\n", szPath, errno);
			}
			break;
		}
	}

	time_t iTime = time(nullptr);
	tm* pTime = localtime(&iTime);

	g_SMAPI->PathFormat(szPath, sizeof(szPath), "%s/addons/logs/fab/fab_%02d_%02d_%04d.txt",
		g_SMAPI->GetBaseDir(), pTime->tm_mday, pTime->tm_mon + 1, pTime->tm_year + 1900);

	FILE* pFile = fopen(szPath, "a");
	if (!pFile)
	{
		if (!bError)
		{
			bError = true;
			Msg("[FAB] Failed to open %s (errno %d)\n", szPath, errno);
		}
		return;
	}

	char szBuffer[1024];
	va_list args;
	va_start(args, szFormat);
	vsnprintf(szBuffer, sizeof(szBuffer), szFormat, args);
	va_end(args);

	fprintf(pFile, "[%02d:%02d:%02d] %s\n", pTime->tm_hour, pTime->tm_min, pTime->tm_sec, szBuffer);
	fclose(pFile);
}

std::vector<std::string> SplitString(const std::string& sText)
{
	std::vector<std::string> vecResult;
	std::string sItem;
	for (size_t i = 0; i <= sText.size(); i++)
	{
		if (i == sText.size() || sText[i] == ',')
		{
			size_t iStart = sItem.find_first_not_of(" \t");
			size_t iEnd = sItem.find_last_not_of(" \t");
			if (iStart != std::string::npos)
				vecResult.push_back(sItem.substr(iStart, iEnd - iStart + 1));
			sItem.clear();
		}
		else
			sItem += sText[i];
	}
	return vecResult;
}

void GetTeamsCount(int& iT, int& iCT, int iSkip = -1)
{
	iT = 0;
	iCT = 0;
	for (int i = 0; i < 64; i++)
	{
		if (i == iSkip || !IsValidPlayer(i))
			continue;
		if (g_iTeam[i] == 2)
			iT++;
		else if (g_iTeam[i] == 3)
			iCT++;
	}
}

bool IsAdmin(int iSlot)
{
	if (!g_bAdminImmunity || !g_pAdminCore)
		return false;
	return g_pAdminCore->HasPermission(iSlot, g_sAdminFlag.c_str());
}

bool IsVip(int iSlot)
{
	if (!g_bVipImmunity || !g_pVIPCore || !g_pVIPCore->VIP_IsClientVIP(iSlot))
		return false;
	if (g_vecVipGroups.empty())
		return true;

	const char* szGroup = g_pVIPCore->VIP_GetClientVIPGroup(iSlot);
	if (!szGroup || !szGroup[0])
		return false;

	for (auto& sGroup : g_vecVipGroups)
	{
		if (sGroup == szGroup)
			return true;
	}
	return false;
}

int GetMaxDiff(int iSlot)
{
	if (IsAdmin(iSlot)) return g_iAdminMaxDiff;
	if (IsVip(iSlot)) return g_iVipMaxDiff;
	return g_iMaxDiff;
}

int GetBlockDiff(int iSlot)
{
	if (IsAdmin(iSlot)) return g_iAdminBlockDiff;
	if (IsVip(iSlot)) return g_iVipBlockDiff;
	return g_iBlockDiff;
}

const char* GetGroup(int iSlot)
{
	if (IsAdmin(iSlot)) return "Admin";
	if (IsVip(iSlot)) return "VIP";
	return "Player";
}

const char* GetTranslation(const char* szKey)
{
	auto it = g_vecPhrases.find(szKey);
	if (it != g_vecPhrases.end())
		return it->second.c_str();

	if (!strcmp(szKey, "FAB_Chat_T"))
		return "{BLUE}[FAB] {DEFAULT}You were transferred to the team {RED}Terrorists {DEFAULT}for balance";
	if (!strcmp(szKey, "FAB_Chat_CT"))
		return "{BLUE}[FAB] {DEFAULT}You were transferred to the team {RED}Counter-Terrorists {DEFAULT}for balance";
	if (!strcmp(szKey, "FAB_Block"))
		return "{BLUE}[FAB] {DEFAULT}You cannot switch to this team, the difference is too big!";
	return szKey;
}

bool MovePlayer(int iSlot, int iTeam, bool bPrestart)
{
	if ((iTeam != 2 && iTeam != 3) || !IsValidPlayer(iSlot) || !g_pPlayers->IsInGame(iSlot))
		return false;

	if (!bPrestart && g_bAlive[iSlot])
	{
		LogDebug("[MOVE] %d (%s) -> %s skipped, player is alive", iSlot, GetClientName(iSlot), GetTeamName(iTeam));
		return false;
	}

	g_bPluginMove[iSlot] = true;
	g_iTeam[iSlot] = iTeam;
	g_pPlayers->SwitchTeam(iSlot, iTeam);
	g_bPluginMove[iSlot] = false;

	LogDebug("[MOVE] %d (%s) -> %s | alive %d | prestart %d", iSlot, GetClientName(iSlot), GetTeamName(iTeam), g_bAlive[iSlot], bPrestart);
	return true;
}

void DisableNativeBalance()
{
	if (!engine)
		return;

	if (!g_bForceNativeOff)
	{
		LogDebug("[NATIVE] force_native_off 0, skip");
		return;
	}

	engine->ServerCommand("mp_autoteambalance 0\n");
	engine->ServerCommand("mp_limitteams 0\n");
	Msg("[FAB] mp_autoteambalance 0, mp_limitteams 0\n");
	LogDebug("[NATIVE] mp_autoteambalance 0, mp_limitteams 0");
}

void LoadConfig()
{
	g_iMaxDiff = 2;
	g_iBlockDiff = 1;
	g_bMessage = true;
	g_bDebug = false;
	g_bForceNativeOff = true;
	g_bAdminImmunity = true;
	g_sAdminFlag = "@admin/balance";
	g_iAdminMaxDiff = 3;
	g_iAdminBlockDiff = 2;
	g_bVipImmunity = true;
	g_vecVipGroups.clear();
	g_iVipMaxDiff = 3;
	g_iVipBlockDiff = 2;

	KeyValues* kv = new KeyValues("fab");
	if (!kv->LoadFromFile(filesystem, "addons/configs/fastautobalance.ini", "GAME"))
	{
		Msg("[FAB] Failed to load addons/configs/fastautobalance.ini, using defaults\n");
		delete kv;
		return;
	}

	g_iMaxDiff = kv->GetInt("MaxAD", 2);
	g_iBlockDiff = kv->GetInt("block", 1);
	g_bMessage = kv->GetBool("msg", true);
	g_bDebug = kv->GetBool("debug", false);
	g_bForceNativeOff = kv->GetBool("force_native_off", true);

	g_bAdminImmunity = kv->GetBool("admin_imune", true);
	g_sAdminFlag = kv->GetString("admin_flags", "@admin/balance");
	g_iAdminMaxDiff = kv->GetInt("admin_max", 3);
	g_iAdminBlockDiff = kv->GetInt("admin_block", 2);

	g_bVipImmunity = kv->GetBool("vip_imune", true);
	g_iVipMaxDiff = kv->GetInt("vip_max", 3);
	g_iVipBlockDiff = kv->GetInt("vip_block", 2);
	g_vecVipGroups = SplitString(kv->GetString("vip_groups", ""));

	delete kv;

	Msg("[FAB] Config loaded: max %d block %d | admin %d %d/%d | vip %d %d/%d | debug %d\n",
		g_iMaxDiff, g_iBlockDiff, g_bAdminImmunity, g_iAdminMaxDiff, g_iAdminBlockDiff,
		g_bVipImmunity, g_iVipMaxDiff, g_iVipBlockDiff, g_bDebug);

	if (g_bDebug)
	{
		Msg("[FAB] Debug log: %s/addons/logs/fab/\n", g_SMAPI->GetBaseDir());

		std::string sGroups;
		for (auto& sGroup : g_vecVipGroups)
			sGroups += (sGroups.empty() ? "" : ",") + sGroup;

		LogDebug("[CONFIG] max %d block %d msg %d native_off %d | admin %d %d/%d %s | vip %d %d/%d %s",
			g_iMaxDiff, g_iBlockDiff, g_bMessage, g_bForceNativeOff,
			g_bAdminImmunity, g_iAdminMaxDiff, g_iAdminBlockDiff, g_sAdminFlag.c_str(),
			g_bVipImmunity, g_iVipMaxDiff, g_iVipBlockDiff, sGroups.empty() ? "all" : sGroups.c_str());
	}
}

void LoadTranslations()
{
	g_vecPhrases.clear();

	KeyValues* kv = new KeyValues("Phrases");
	if (!kv->LoadFromFile(filesystem, "addons/translations/fab_phrases.txt", "GAME"))
	{
		Msg("[FAB] Failed to load addons/translations/fab_phrases.txt\n");
		delete kv;
		return;
	}

	const char* szLanguage = g_pUtils ? g_pUtils->GetLanguage() : "en";
	FOR_EACH_SUBKEY(kv, pKey)
	{
		const char* szText = pKey->GetString(szLanguage, "");
		if (szText[0])
			g_vecPhrases[pKey->GetName()] = szText;
	}

	delete kv;
}

void CheckDeath(int iSlot, int iT, int iCT, int iTeam)
{
	int iMax = GetMaxDiff(iSlot);

	if (iTeam == 2 && iT > iCT && iT - iCT > iMax)
	{
		g_Queue[iSlot] = { 3, 2, g_iRound };
		LogDebug("[DEATH] %d (%s) %s died T | T %d CT %d | diff %d > %d | will move to CT (round %d)",
			iSlot, GetClientName(iSlot), GetGroup(iSlot), iT, iCT, iT - iCT, iMax, g_iRound);
		return;
	}

	if (iTeam == 3 && iCT > iT && iCT - iT > iMax)
	{
		g_Queue[iSlot] = { 2, 3, g_iRound };
		LogDebug("[DEATH] %d (%s) %s died CT | T %d CT %d | diff %d > %d | will move to T (round %d)",
			iSlot, GetClientName(iSlot), GetGroup(iSlot), iT, iCT, iCT - iT, iMax, g_iRound);
		return;
	}

	LogDebug("[DEATH] %d (%s) died %s | T %d CT %d | diff %d <= %d",
		iSlot, GetClientName(iSlot), GetTeamName(iTeam), iT, iCT, abs(iT - iCT), iMax);
}

void FastAutoBalance::Hook_ClientCommand(CPlayerSlot slot, const CCommand& args)
{
	int iSlot = slot.Get();
	if (!IsValidPlayer(iSlot) || args.ArgC() < 2 || strcmp(args.Arg(0), "jointeam"))
		RETURN_META(MRES_IGNORED);

	int iNewTeam = atoi(args.Arg(1));
	int iTeam = g_iTeam[iSlot];
	if ((iNewTeam != 2 && iNewTeam != 3) || iNewTeam == iTeam || (iTeam != 2 && iTeam != 3))
		RETURN_META(MRES_IGNORED);

	int iT, iCT;
	GetTeamsCount(iT, iCT, iSlot);
	if (iNewTeam == 2)
		iT++;
	else
		iCT++;

	int iDiff = abs(iT - iCT);
	int iBlock = GetBlockDiff(iSlot);

	if (iDiff <= iBlock)
	{
		LogDebug("[JOIN] %d (%s) %s | %s -> %s | T %d CT %d | diff %d <= %d | allowed",
			iSlot, GetClientName(iSlot), GetGroup(iSlot), GetTeamName(iTeam), GetTeamName(iNewTeam), iT, iCT, iDiff, iBlock);
		RETURN_META(MRES_IGNORED);
	}

	time_t iTime = time(nullptr);
	if (iTime - g_iBlockMsgTime[iSlot] >= 2)
	{
		LogDebug("[JOIN] %d (%s) %s | %s -> %s | T %d CT %d | diff %d > %d | blocked (+%d)",
			iSlot, GetClientName(iSlot), GetGroup(iSlot), GetTeamName(iTeam), GetTeamName(iNewTeam), iT, iCT, iDiff, iBlock, g_iBlockMsgSkip[iSlot]);

		if (g_bMessage && g_pUtils)
			g_pUtils->PrintToChat(iSlot, " %s", GetTranslation("FAB_Block"));

		g_iBlockMsgTime[iSlot] = iTime;
		g_iBlockMsgSkip[iSlot] = 0;
	}
	else
		g_iBlockMsgSkip[iSlot]++;

	RETURN_META(MRES_SUPERCEDE);
}

void OnPlayerTeam(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if (iSlot < 0 || iSlot >= 64)
		return;

	int iTeam = pEvent->GetInt("team");
	int iOldTeam = pEvent->GetInt("oldteam");

	if (iTeam <= 1)
		g_bAlive[iSlot] = false;

	int iCached = g_iTeam[iSlot];
	g_iTeam[iSlot] = iTeam;

	auto it = g_Queue.find(iSlot);
	if (it != g_Queue.end() && iTeam != it->second.iOldTeam)
	{
		LogDebug("[TEAM] %d (%s) %s -> %s | removed from queue", iSlot, GetClientName(iSlot), GetTeamName(iOldTeam), GetTeamName(iTeam));
		g_Queue.erase(it);
	}

	if (g_bDebug)
	{
		int iT, iCT;
		GetTeamsCount(iT, iCT);
		LogDebug("[TEAM] %d (%s) %s -> %s by %s | T %d CT %d (was %d)",
			iSlot, GetClientName(iSlot), GetTeamName(iOldTeam), GetTeamName(iTeam), g_bPluginMove[iSlot] ? "FAB" : "game", iT, iCT, iCached);
	}
}

void OnPlayerDeath(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if (iSlot < 0 || iSlot >= 64)
		return;

	g_bAlive[iSlot] = false;

	int iTeam = g_iTeam[iSlot];
	if (!IsValidPlayer(iSlot) || (iTeam != 2 && iTeam != 3))
		return;

	int iT, iCT;
	GetTeamsCount(iT, iCT);
	if (iTeam == 2)
		iT--;
	else
		iCT--;

	CheckDeath(iSlot, iT, iCT, iTeam);
}

void OnPlayerSpawn(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if (iSlot < 0 || iSlot >= 64)
		return;

	g_bAlive[iSlot] = true;

	auto it = g_Queue.find(iSlot);
	if (it != g_Queue.end() && it->second.iRound == g_iRound)
	{
		LogDebug("[SPAWN] %d (%s) respawned this round, removed from queue", iSlot, GetClientName(iSlot));
		g_Queue.erase(it);
	}
}

void BalanceTeams(bool bPrestart)
{
	g_iRound++;

	int iT, iCT;
	GetTeamsCount(iT, iCT);
	LogDebug("[ROUND] %d (%s) | T %d CT %d | queue %d", g_iRound, bPrestart ? "round_prestart" : "round_start", iT, iCT, (int)g_Queue.size());

	if (g_Queue.empty())
		return;

	std::vector<int> vecSlots;
	for (auto& it : g_Queue)
		vecSlots.push_back(it.first);

	for (int iSlot : vecSlots)
	{
		auto it = g_Queue.find(iSlot);
		if (it == g_Queue.end())
			continue;

		BalanceInfo info = it->second;
		g_Queue.erase(it);

		if (!IsValidPlayer(iSlot))
		{
			LogDebug("[ROUND] %d left the server, skip", iSlot);
			continue;
		}

		if (g_iTeam[iSlot] != info.iOldTeam)
		{
			LogDebug("[ROUND] %d (%s) is %s now (died as %s), skip", iSlot, GetClientName(iSlot), GetTeamName(g_iTeam[iSlot]), GetTeamName(info.iOldTeam));
			continue;
		}

		GetTeamsCount(iT, iCT);
		int iMax = GetMaxDiff(iSlot);

		bool bNeed = (info.iNewTeam == 3 && iT > iCT && iT - iCT > iMax) || (info.iNewTeam == 2 && iCT > iT && iCT - iT > iMax);
		if (!bNeed)
		{
			LogDebug("[ROUND] %d (%s) %s | T %d CT %d | diff %d <= %d | not needed", iSlot, GetClientName(iSlot), GetGroup(iSlot), iT, iCT, abs(iT - iCT), iMax);
			continue;
		}

		int iNewT = info.iNewTeam == 2 ? iT + 1 : iT - 1;
		int iNewCT = info.iNewTeam == 3 ? iCT + 1 : iCT - 1;

		if (iNewT <= 0 || iNewCT <= 0)
		{
			LogDebug("[ROUND] %d (%s) | team would be empty (T %d CT %d), skip", iSlot, GetClientName(iSlot), iNewT, iNewCT);
			continue;
		}

		if (abs(iNewT - iNewCT) >= abs(iT - iCT))
		{
			LogDebug("[ROUND] %d (%s) | diff does not change (%d -> %d), skip", iSlot, GetClientName(iSlot), abs(iT - iCT), abs(iNewT - iNewCT));
			continue;
		}

		if (!MovePlayer(iSlot, info.iNewTeam, bPrestart))
		{
			LogDebug("[ROUND] %d (%s) move failed", iSlot, GetClientName(iSlot));
			continue;
		}

		LogDebug("[ROUND] %d (%s) %s | T %d CT %d | moved %s -> %s",
			iSlot, GetClientName(iSlot), GetGroup(iSlot), iT, iCT, GetTeamName(info.iOldTeam), GetTeamName(info.iNewTeam));
		Msg("[FAB] %s moved to %s (T %d CT %d)\n", GetClientName(iSlot), GetTeamName(info.iNewTeam), iT, iCT);

		if (g_bMessage && g_pUtils)
			g_pUtils->PrintToChat(iSlot, " %s", GetTranslation(info.iNewTeam == 3 ? "FAB_Chat_CT" : "FAB_Chat_T"));
	}
}

void OnRoundPrestart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	g_bPrestart = true;
	BalanceTeams(true);
}

void OnRoundStart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (g_bPrestart)
	{
		g_bPrestart = false;
		return;
	}
	BalanceTeams(false);
}

void OnPlayerConnectFull(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if (iSlot < 0 || iSlot >= 64)
		return;

	ResetPlayer(iSlot);
}

void OnPlayerDisconnect(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if (iSlot < 0 || iSlot >= 64)
		return;

	if (g_Queue.erase(iSlot))
		LogDebug("[DISCONNECT] %d (%s) removed from queue", iSlot, GetClientName(iSlot));

	ResetPlayer(iSlot);
}

float CheckPlayers()
{
	for (int i = 0; i < 64; i++)
	{
		if (!IsValidPlayer(i))
		{
			g_iTeam[i] = 0;
			g_bAlive[i] = false;
		}
	}
	return 1.0f;
}

bool OnReloadCommand(int iSlot, const char* szContent)
{
	if (iSlot >= 0 && (!g_pAdminCore || !g_pAdminCore->HasPermission(iSlot, g_sAdminFlag.c_str())))
		return true;

	LoadConfig();
	LoadTranslations();
	DisableNativeBalance();

	LogDebug("[RELOAD] by %d (%s)", iSlot, iSlot >= 0 ? GetClientName(iSlot) : "Console");

	if (iSlot >= 0)
		g_pUtils->PrintToChat(iSlot, " \x0B[FAB] \x04Config reloaded");
	else
		Msg("[FAB] Config reloaded\n");
	return true;
}

void OnStartupServer()
{
	g_pGameEntitySystem = g_pUtils->GetCGameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();

	LoadConfig();
	LoadTranslations();
	DisableNativeBalance();

	g_iRound = 0;
	g_bPrestart = false;
	g_Queue.clear();
	for (int i = 0; i < 64; i++)
		ResetPlayer(i);

	LogDebug("[START] FastAutoBalance %s", g_FastAutoBalance.GetVersion());
}

bool FastAutoBalance::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	g_bLateLoad = late;

	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, filesystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pGameClients, ISource2GameClients, INTERFACEVERSION_SERVERGAMECLIENTS);

	SH_ADD_HOOK(ISource2GameClients, ClientCommand, g_pGameClients, SH_MEMBER(this, &FastAutoBalance::Hook_ClientCommand), false);

	g_SMAPI->AddListener(this, this);
	return true;
}

bool FastAutoBalance::Unload(char* error, size_t maxlen)
{
	SH_REMOVE_HOOK(ISource2GameClients, ClientCommand, g_pGameClients, SH_MEMBER(this, &FastAutoBalance::Hook_ClientCommand), false);

	if (g_pUtils)
	{
		if (g_pCheckTimer)
		{
			g_pUtils->RemoveTimer(g_pCheckTimer);
			g_pCheckTimer = nullptr;
		}
		g_pUtils->ClearAllHooks(g_PLID);
	}

	g_vecPhrases.clear();
	g_vecVipGroups.clear();
	g_Queue.clear();
	return true;
}

void FastAutoBalance::AllPluginsLoaded()
{
	int ret;
	g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		Msg("[FAB] Missing Utils plugin\n");
		return;
	}

	g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		Msg("[FAB] Missing Players plugin\n");
		return;
	}

	g_pVIPCore = (IVIPApi*)g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
		g_pVIPCore = nullptr;

	g_pAdminCore = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
		g_pAdminCore = nullptr;

	for (int i = 0; i < 64; i++)
		ResetPlayer(i);

	g_pUtils->StartupServer(g_PLID, OnStartupServer);
	if (g_bLateLoad)
		OnStartupServer();

	g_pUtils->HookEvent(g_PLID, "player_team", OnPlayerTeam);
	g_pUtils->HookEvent(g_PLID, "player_death", OnPlayerDeath);
	g_pUtils->HookEvent(g_PLID, "player_spawn", OnPlayerSpawn);
	g_pUtils->HookEvent(g_PLID, "round_prestart", OnRoundPrestart);
	g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStart);
	g_pUtils->HookEvent(g_PLID, "player_connect_full", OnPlayerConnectFull);
	g_pUtils->HookEvent(g_PLID, "player_disconnect", OnPlayerDisconnect);

	g_pUtils->RegCommand(g_PLID, { "mm_fab_reload", "fab_reload" }, {}, OnReloadCommand);

	g_pCheckTimer = g_pUtils->CreateTimer(1.0f, CheckPlayers);
}

const char* FastAutoBalance::GetLicense()
{
	return "Public";
}

const char* FastAutoBalance::GetVersion()
{
	return "3.0";
}

const char* FastAutoBalance::GetDate()
{
	return __DATE__;
}

const char* FastAutoBalance::GetLogTag()
{
	return "[FAB]";
}

const char* FastAutoBalance::GetAuthor()
{
	return "_ded_cookies";
}

const char* FastAutoBalance::GetDescription()
{
	return "Team Auto Balance";
}

const char* FastAutoBalance::GetName()
{
	return "FastAutoBalance";
}

const char* FastAutoBalance::GetURL()
{
	return "https://api.onlypublic.net/";
}
