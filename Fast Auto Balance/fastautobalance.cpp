#include "fastautobalance.h"

FastAutoBalance g_FastAutoBalance;
PLUGIN_EXPOSE(FastAutoBalance, g_FastAutoBalance);

PLUGIN_GLOBALVARS();

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;
IVIPApi* g_pVIPCore = nullptr;
IAdminApi* g_pAdminCore = nullptr;
IVEngineServer2* engine = nullptr;
IFileSystem* filesystem = nullptr;

std::map<std::string, std::string> g_Phrases;

int g_iMaxAD = 2;
int g_iBlock = 1;
bool g_bShowMsg = true;
bool g_bDebug = false;

bool g_bAdminImmune = true;
std::string g_sAdminFlag = "@admin/balance";
int g_iAdminMax = 3;
int g_iAdminBlock = 2;

bool g_bVIPImmune = true;
std::vector<std::string> g_vecVIPGroups;
int g_iVIPMax = 3;
int g_iVIPBlock = 2;

int g_iTeam[64] = {0};
bool g_bChangingTeam[64] = {false};

struct PendingBalance {
	int targetTeam;
	int deathTeam;
	int deathRound;
};
std::map<int, PendingBalance> g_PendingBalance;
int g_iCurrentRound = 0;

void LogFAB(const char* format, ...)
{
	if (!g_pUtils) return;
	
	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	
	g_pUtils->LogToFile("FAB", "%s", buffer);
}

const char* GetTranslation(const char* key)
{
	auto it = g_Phrases.find(key);
	if (it != g_Phrases.end())
		return it->second.c_str();
	
	if (!strcmp(key, "FAB_Chat_T"))
		return "{BLUE}[FAB] {DEFAULT}You were transferred to the team {RED}Terrorists {DEFAULT}for balance";
	if (!strcmp(key, "FAB_Chat_CT"))
		return "{BLUE}[FAB] {DEFAULT}You were transferred to the team {RED}Counter-Terrorists {DEFAULT}for balance";
	if (!strcmp(key, "FAB_Block"))
		return "{BLUE}[FAB] {DEFAULT}You cannot switch to this team, the difference is too big!";
	
	return key;
}

void LoadTranslations()
{
	g_Phrases.clear();
	
	KeyValues* kv = new KeyValues("Phrases");
	const char* path = "addons/translations/fab_phrases.txt";
	
	if (!kv->LoadFromFile(filesystem, path, "GAME"))
	{
		Msg("[FAB] Failed to load translations from %s, using defaults\n", path);
		delete kv;
		return;
	}
	
	const char* language = g_pUtils ? g_pUtils->GetLanguage() : "en";
	
	FOR_EACH_SUBKEY(kv, pSubKey)
	{
		const char* keyName = pSubKey->GetName();
		const char* translation = pSubKey->GetString(language, "");
		
		if (translation && translation[0])
		{
			g_Phrases[keyName] = translation;
		}
	}
	
	delete kv;
	
	Msg("[FAB] Loaded translations for language: %s\n", language);
}

void GetCounts(int& t, int& ct, int excludeSlot = -1)
{
	t = ct = 0;
	for (int i = 0; i < 64; i++)
	{
		if (i == excludeSlot) continue;
		if (!g_pPlayers || !g_pPlayers->IsConnected(i) || g_pPlayers->IsFakeClient(i))
			continue;
		if (g_iTeam[i] == 2) t++;
		else if (g_iTeam[i] == 3) ct++;
	}
}

std::vector<std::string> SplitString(const std::string& str, const std::string& delimiter)
{
	std::vector<std::string> result;
	size_t start = 0;
	size_t end = str.find(delimiter);
	
	while (end != std::string::npos)
	{
		result.push_back(str.substr(start, end - start));
		start = end + delimiter.length();
		end = str.find(delimiter, start);
	}
	
	result.push_back(str.substr(start));
	return result;
}

bool HasAdminPermission(int iSlot)
{
	if (!g_bAdminImmune || !g_pAdminCore) return false;
	return g_pAdminCore->HasPermission(iSlot, g_sAdminFlag.c_str());
}

bool HasVIPImmunity(int iSlot)
{
	if (!g_bVIPImmune || !g_pVIPCore) return false;
	if (!g_pVIPCore->VIP_IsClientVIP(iSlot)) return false;
	
	if (g_vecVIPGroups.empty()) return true;
	
	const char* playerGroup = g_pVIPCore->VIP_GetClientVIPGroup(iSlot);
	if (!playerGroup) return false;
	
	for (const auto& group : g_vecVIPGroups)
	{
		if (group == playerGroup)
			return true;
	}
	
	return false;
}

void LoadConfig()
{
	KeyValues* kv = new KeyValues("fab");
	const char* configPath = "addons/configs/fastautobalance.ini";
	
	if (!kv->LoadFromFile(filesystem, configPath, "GAME"))
	{
		Msg("[FAB] Config not found at %s, using defaults\n", configPath);
		delete kv;
		return;
	}
	
	g_iMaxAD = kv->GetInt("MaxAD", 2);
	g_iBlock = kv->GetInt("block", 1);
	g_bShowMsg = kv->GetBool("msg", true);
	g_bDebug = kv->GetBool("debug", false);
	
	g_bAdminImmune = kv->GetBool("admin_imune", true);
	g_sAdminFlag = kv->GetString("admin_flags", "@admin/balance");
	g_iAdminMax = kv->GetInt("admin_max", 3);
	g_iAdminBlock = kv->GetInt("admin_block", 2);
	
	g_bVIPImmune = kv->GetBool("vip_imune", true);
	g_iVIPMax = kv->GetInt("vip_max", 3);
	g_iVIPBlock = kv->GetInt("vip_block", 2);
	
	const char* vipGroups = kv->GetString("vip_groups", "");
	if (vipGroups && *vipGroups)
	{
		g_vecVIPGroups = SplitString(std::string(vipGroups), ",");
	}
	
	delete kv;
	
	Msg("[FAB] Config loaded: MaxAD=%d, Block=%d, AdminMax=%d, AdminBlock=%d, VIPMax=%d, VIPBlock=%d, Debug=%s\n", 
		g_iMaxAD, g_iBlock, g_iAdminMax, g_iAdminBlock, g_iVIPMax, g_iVIPBlock, g_bDebug ? "ON" : "OFF");
	
	if (g_bDebug)
	{
		std::string vipGroupsStr = "all";
		if (!g_vecVIPGroups.empty())
		{
			vipGroupsStr = "";
			for (size_t i = 0; i < g_vecVIPGroups.size(); i++)
			{
				if (i > 0) vipGroupsStr += ",";
				vipGroupsStr += g_vecVIPGroups[i];
			}
		}
		
		LogFAB("[CONFIG] MaxAD=%d block=%d msg=%d debug=%d | Admin: imune=%d max=%d block=%d flags=%s | VIP: imune=%d max=%d block=%d groups=%s",
			g_iMaxAD, g_iBlock, g_bShowMsg, g_bDebug,
			g_bAdminImmune, g_iAdminMax, g_iAdminBlock, g_sAdminFlag.c_str(),
			g_bVIPImmune, g_iVIPMax, g_iVIPBlock, 
			vipGroupsStr.c_str());
	}
}

void OnPlayerTeamPre(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent || !g_pPlayers || !g_pUtils) return;
	
	int slot = pEvent->GetInt("userid");
	int newTeam = pEvent->GetInt("team");
	int oldTeam = pEvent->GetInt("oldteam");
	bool disconnect = pEvent->GetBool("disconnect");
	
	if (slot < 0 || slot >= 64) return;
	if (g_pPlayers->IsFakeClient(slot)) return;
	if (disconnect) return;
	if (newTeam <= 1 || oldTeam <= 1) return;
	if (newTeam == oldTeam) return;

	if (g_bChangingTeam[slot])
	{
		g_bChangingTeam[slot] = false;
		return;
	}

	int t, ct;
	GetCounts(t, ct, slot);

	if (newTeam == 2) t++;
	else if (newTeam == 3) ct++;
	
	int diff = abs(t - ct);
	int maxDiff = g_iBlock;
	
	if (HasAdminPermission(slot))
		maxDiff = g_iAdminBlock;
	else if (HasVIPImmunity(slot))
		maxDiff = g_iVIPBlock;
	
	if (diff > maxDiff)
	{
		g_pPlayers->SwitchTeam(slot, oldTeam);
		
		if (g_bDebug)
		{
			const char* playerName = g_pPlayers->GetPlayerName(slot);
			LogFAB("[BLOCK] slot %d (%s) tried %s->%s | Would be T=%d CT=%d diff=%d > maxDiff=%d | BLOCKED",
				slot, playerName ? playerName : "Unknown",
				oldTeam == 2 ? "T" : "CT",
				newTeam == 2 ? "T" : "CT",
				t, ct, diff, maxDiff);
		}
		
		if (g_bShowMsg)
		{
			const char* msg = GetTranslation("FAB_Block");
			g_pUtils->PrintToChat(slot, " %s", msg);
		}
	}
	else if (g_bDebug)
	{
		const char* playerName = g_pPlayers->GetPlayerName(slot);
		LogFAB("[ALLOW] slot %d (%s) switch %s->%s | Will be T=%d CT=%d diff=%d <= maxDiff=%d | ALLOWED",
			slot, playerName ? playerName : "Unknown",
			oldTeam == 2 ? "T" : "CT",
			newTeam == 2 ? "T" : "CT",
			t, ct, diff, maxDiff);
	}
}

void OnPlayerTeam(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent) return;
	int slot = pEvent->GetInt("userid");
	int team = pEvent->GetInt("team");
	if (slot >= 0 && slot < 64) g_iTeam[slot] = team;
}

void OnPlayerDeath(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent || !g_pPlayers || !g_pUtils) return;
	
	int slot = pEvent->GetInt("userid");
	if (slot < 0 || slot >= 64) return;
	if (!g_pPlayers->IsConnected(slot) || g_pPlayers->IsFakeClient(slot)) return;
	
	int team = g_iTeam[slot];
	if (team <= 1) return;
	
	int t, ct;
	GetCounts(t, ct, slot);
	
	int maxDiff = g_iMaxAD;
	if (HasAdminPermission(slot))
		maxDiff = g_iAdminMax;
	else if (HasVIPImmunity(slot))
		maxDiff = g_iVIPMax;
	
	if (t > ct && (t - ct) > maxDiff && team == 2)
	{
		if (t <= 1)
		{
			if (g_bDebug)
			{
				const char* playerName = g_pPlayers->GetPlayerName(slot);
				LogFAB("[DEATH] slot %d (%s) died in T | T=%d CT=%d | SAFETY: Would create empty T team | NOT marked",
					slot, playerName ? playerName : "Unknown", t, ct);
			}
			return;
		}
		
		PendingBalance pending;
		pending.targetTeam = 3;
		pending.deathTeam = 2;
		pending.deathRound = g_iCurrentRound;
		g_PendingBalance[slot] = pending;
		
		if (g_bDebug)
		{
			const char* playerName = g_pPlayers->GetPlayerName(slot);
			const char* playerType = HasAdminPermission(slot) ? "Admin" : (HasVIPImmunity(slot) ? "VIP" : "Player");
			LogFAB("[DEATH] slot %d (%s) %s died in T | T=%d CT=%d diff=%d > maxDiff=%d | MARKED for CT (round %d)",
				slot, playerName ? playerName : "Unknown", playerType,
				t, ct, abs(t - ct), maxDiff, g_iCurrentRound);
		}
		
		Msg("[FAB] Marked slot %d for balance: T->CT (round %d)\n", slot, g_iCurrentRound);
	}
	else if (ct > t && (ct - t) > maxDiff && team == 3)
	{
		if (ct <= 1)
		{
			if (g_bDebug)
			{
				const char* playerName = g_pPlayers->GetPlayerName(slot);
				LogFAB("[DEATH] slot %d (%s) died in CT | T=%d CT=%d | SAFETY: Would create empty CT team | NOT marked",
					slot, playerName ? playerName : "Unknown", t, ct);
			}
			return;
		}
		
		PendingBalance pending;
		pending.targetTeam = 2;
		pending.deathTeam = 3;
		pending.deathRound = g_iCurrentRound;
		g_PendingBalance[slot] = pending;
		
		if (g_bDebug)
		{
			const char* playerName = g_pPlayers->GetPlayerName(slot);
			const char* playerType = HasAdminPermission(slot) ? "Admin" : (HasVIPImmunity(slot) ? "VIP" : "Player");
			LogFAB("[DEATH] slot %d (%s) %s died in CT | T=%d CT=%d diff=%d > maxDiff=%d | MARKED for T (round %d)",
				slot, playerName ? playerName : "Unknown", playerType,
				t, ct, abs(ct - t), maxDiff, g_iCurrentRound);
		}
		
		Msg("[FAB] Marked slot %d for balance: CT->T (round %d)\n", slot, g_iCurrentRound);
	}
	else if (g_bDebug)
	{
		const char* playerName = g_pPlayers->GetPlayerName(slot);
		LogFAB("[DEATH] slot %d (%s) died in %s | T=%d CT=%d diff=%d <= maxDiff=%d | NOT marked",
			slot, playerName ? playerName : "Unknown",
			team == 2 ? "T" : "CT",
			t, ct, abs(t - ct), maxDiff);
	}
}

void OnPlayerSpawn(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent || !g_pPlayers || !g_pUtils) return;
	
	int slot = pEvent->GetInt("userid");
	if (slot < 0 || slot >= 64) return;
	
	auto it = g_PendingBalance.find(slot);
	if (it != g_PendingBalance.end())
	{
		if (g_bDebug)
		{
			const char* playerName = g_pPlayers->GetPlayerName(slot);
			const char* toTeam = it->second.targetTeam == 2 ? "T" : "CT";
			
			LogFAB("[SPAWN] slot %d (%s) | WARNING: Still in pending (target %s) | REMOVED (should have been transferred on round_start)",
				slot, playerName ? playerName : "Unknown", toTeam);
		}
		
		g_PendingBalance.erase(slot);
	}
}

void OnRoundStart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	g_iCurrentRound++;
	
	int pendingCount = g_PendingBalance.size();
	
	int t, ct;
	GetCounts(t, ct);
	
	if (g_bDebug)
	{
		LogFAB("[ROUND] Round %d started | T=%d CT=%d diff=%d | %d player(s) pending balance",
			g_iCurrentRound, t, ct, abs(t - ct), pendingCount);
	}
	
	Msg("[FAB] Round %d started\n", g_iCurrentRound);
	
	if (pendingCount > 0)
	{
		std::vector<int> pendingSlots;
		for (const auto& pair : g_PendingBalance)
		{
			pendingSlots.push_back(pair.first);
		}
		
		for (int slot : pendingSlots)
		{
			auto it = g_PendingBalance.find(slot);
			if (it == g_PendingBalance.end())
				continue;
			
			PendingBalance pending = it->second;
			const char* playerName = g_pPlayers ? g_pPlayers->GetPlayerName(slot) : "Unknown";
			
			if (!g_pPlayers || !g_pPlayers->IsConnected(slot) || g_pPlayers->IsFakeClient(slot))
			{
				g_PendingBalance.erase(slot);
				if (g_bDebug)
				{
					LogFAB("[ROUND] slot %d (%s) | CHECK FAILED: not connected | REMOVED from pending",
						slot, playerName ? playerName : "Unknown");
				}
				continue;
			}
			
			int currentTeam = g_iTeam[slot];
			if (currentTeam != pending.deathTeam)
			{
				g_PendingBalance.erase(slot);
				if (g_bDebug)
				{
					LogFAB("[ROUND] slot %d (%s) | CHECK FAILED: team changed (now %s, died in %s) | REMOVED from pending",
						slot, playerName ? playerName : "Unknown",
						currentTeam == 2 ? "T" : (currentTeam == 3 ? "CT" : "SPEC"),
						pending.deathTeam == 2 ? "T" : "CT");
				}
				continue;
			}
			
			int t_now, ct_now;
			GetCounts(t_now, ct_now);
			
			int maxDiff = g_iMaxAD;
			bool isAdmin = HasAdminPermission(slot);
			bool isVIP = HasVIPImmunity(slot);
			
			if (isAdmin)
				maxDiff = g_iAdminMax;
			else if (isVIP)
				maxDiff = g_iVIPMax;
			
			const char* playerType = isAdmin ? "Admin" : (isVIP ? "VIP" : "Player");
			
			bool needBalance = false;
			int currentDiff = abs(t_now - ct_now);
			
			if (pending.targetTeam == 3)
			{
				if (t_now > ct_now && (t_now - ct_now) > maxDiff)
					needBalance = true;
			}
			else if (pending.targetTeam == 2)
			{
				if (ct_now > t_now && (ct_now - t_now) > maxDiff)
					needBalance = true;
			}
			
			if (!needBalance)
			{
				g_PendingBalance.erase(slot);
				if (g_bDebug)
				{
					LogFAB("[ROUND] slot %d (%s) %s | T=%d CT=%d diff=%d <= maxDiff=%d | BALANCE NOT NEEDED | REMOVED from pending",
						slot, playerName ? playerName : "Unknown", playerType,
						t_now, ct_now, currentDiff, maxDiff);
				}
				continue;
			}
			
			int t_after = t_now;
			int ct_after = ct_now;
			
			if (pending.targetTeam == 3)
			{
				t_after = t_now - 1;
				ct_after = ct_now + 1;
			}
			else if (pending.targetTeam == 2)
			{
				t_after = t_now + 1;
				ct_after = ct_now - 1;
			}
			
			if (t_after <= 0 || ct_after <= 0)
			{
				g_PendingBalance.erase(slot);
				if (g_bDebug)
				{
					LogFAB("[ROUND] slot %d (%s) %s | SAFETY: Would create empty team (T=%d CT=%d after transfer) | REMOVED from pending",
						slot, playerName ? playerName : "Unknown", playerType,
						t_after, ct_after);
				}
				continue;
			}
			
			int diff_after = abs(t_after - ct_after);
			
			if (diff_after >= currentDiff)
			{
				g_PendingBalance.erase(slot);
				if (g_bDebug)
				{
					LogFAB("[ROUND] slot %d (%s) %s | SAFETY: Would not improve balance (diff now=%d, after=%d) | REMOVED from pending",
						slot, playerName ? playerName : "Unknown", playerType,
						currentDiff, diff_after);
				}
				continue;
			}
			
			const char* fromTeam = pending.deathTeam == 2 ? "T" : "CT";
			const char* toTeam = pending.targetTeam == 2 ? "T" : "CT";
			
			g_bChangingTeam[slot] = true;
			g_pPlayers->SwitchTeam(slot, pending.targetTeam);
			g_iTeam[slot] = pending.targetTeam;
			
			if (g_bDebug)
			{
				LogFAB("[ROUND] slot %d (%s) %s | T=%d CT=%d diff=%d > maxDiff=%d | TRANSFERRED %s->%s | SUCCESS",
					slot, playerName ? playerName : "Unknown", playerType,
					t_now, ct_now, currentDiff, maxDiff, fromTeam, toTeam);
			}
			
			Msg("[FAB] BALANCED: slot %d -> team %d (T=%d CT=%d)\n", 
				slot, pending.targetTeam, t_now, ct_now);
			
			if (g_bShowMsg)
			{
				const char* msg = (pending.targetTeam == 3) ? 
					GetTranslation("FAB_Chat_CT") : 
					GetTranslation("FAB_Chat_T");
				g_pUtils->PrintToChat(slot, " %s", msg);
			}
			
			g_PendingBalance.erase(slot);
		}
	}
}

void OnPlayerConnect(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent) return;
	int slot = pEvent->GetInt("userid");
	if (slot >= 0 && slot < 64)
	{
		g_iTeam[slot] = 0;
		g_bChangingTeam[slot] = false;
	}
}

void OnPlayerDisconnect(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent) return;
	int slot = pEvent->GetInt("userid");
	if (slot >= 0 && slot < 64)
	{
		auto it = g_PendingBalance.find(slot);
		if (it != g_PendingBalance.end())
		{
			if (g_bDebug)
			{
				const char* playerName = g_pPlayers ? g_pPlayers->GetPlayerName(slot) : "Unknown";
				const char* toTeam = it->second.targetTeam == 2 ? "T" : "CT";
				
				g_PendingBalance.erase(slot);
				
				LogFAB("[DISCONNECT] slot %d (%s) disconnected | Was marked for %s | REMOVED from pending",
					slot, playerName ? playerName : "Unknown", toTeam);
			}
			else
			{
				g_PendingBalance.erase(slot);
			}
			
			Msg("[FAB] Removed slot %d from pending: disconnected\n", slot);
		}
		
		g_iTeam[slot] = 0;
		g_bChangingTeam[slot] = false;
	}
}

bool OnReloadCommand(int iSlot, const char* szContent)
{
	LoadConfig();
	LoadTranslations();
	
	if (g_bDebug)
	{
		const char* playerName = g_pPlayers ? g_pPlayers->GetPlayerName(iSlot) : "Unknown";
		LogFAB("[RELOAD] Config reloaded by slot %d (%s)", iSlot, playerName ? playerName : "Unknown");
	}
	
	g_pUtils->PrintToChat(iSlot, " \x0B[FAB] \x04Config and translations reloaded!");
	return true;
}

void OnStartupServer()
{
	LoadConfig();
	LoadTranslations();
	g_iCurrentRound = 0;
	g_PendingBalance.clear();
	
	if (g_bDebug)
	{
		LogFAB("========================================");
		LogFAB("[SERVER] FastAutoBalance v1.3.2 started");
		LogFAB("========================================");
	}
}

bool FastAutoBalance::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, filesystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	
	Msg("[FAB] Loading v1.3.2...\n");
	return true;
}

bool FastAutoBalance::Unload(char *error, size_t maxlen)
{
	if (g_pUtils) g_pUtils->ClearAllHooks(g_PLID);
	
	g_Phrases.clear();
	g_vecVIPGroups.clear();
	g_PendingBalance.clear();
	
	Msg("[FAB] Unloaded\n");
	
	return true;
}

void FastAutoBalance::AllPluginsLoaded()
{
	Msg("[FAB] AllPluginsLoaded() v1.3.2\n");
	
	int ret;
	
	g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		Msg("[FAB] ERROR: Utils API not found\n");
		return;
	}
	
	g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		Msg("[FAB] ERROR: Players API not found\n");
		return;
	}
	
	g_pVIPCore = (IVIPApi*)g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pVIPCore = nullptr;
		Msg("[FAB] VIP API not found (optional)\n");
	}
	
	g_pAdminCore = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pAdminCore = nullptr;
		Msg("[FAB] Admin API not found (optional)\n");
	}
	
	for (int i = 0; i < 64; i++)
	{
		g_iTeam[i] = 0;
		g_bChangingTeam[i] = false;
	}
	
	g_pUtils->StartupServer(g_PLID, OnStartupServer);
	
	Msg("[FAB] Hooking events...\n");
	g_pUtils->HookEvent(g_PLID, "player_team", OnPlayerTeamPre);
	g_pUtils->HookEvent(g_PLID, "player_team", OnPlayerTeam);
	g_pUtils->HookEvent(g_PLID, "player_death", OnPlayerDeath);
	g_pUtils->HookEvent(g_PLID, "player_spawn", OnPlayerSpawn);
	g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStart);
	g_pUtils->HookEvent(g_PLID, "player_connect_full", OnPlayerConnect);
	g_pUtils->HookEvent(g_PLID, "player_disconnect", OnPlayerDisconnect);
	
	g_pUtils->RegCommand(g_PLID, {"fab_reload"}, {}, OnReloadCommand);
	
	Msg("[FAB] Loaded successfully!\n");
}

///////////////////////////////////////
const char *FastAutoBalance::GetLicense()
{
	return "Public";
}

const char *FastAutoBalance::GetVersion()
{
	return "1.3.2";
}

const char *FastAutoBalance::GetDate()
{
	return __DATE__;
}

const char *FastAutoBalance::GetLogTag()
{
	return "[FAB]";
}

const char *FastAutoBalance::GetAuthor()
{
	return "_ded_cookies";
}

const char *FastAutoBalance::GetDescription()
{
	return "Team Auto Balance";
}

const char *FastAutoBalance::GetName()
{
	return "FastAutoBalance";
}

const char *FastAutoBalance::GetURL()
{
	return "https://api.onlypublic.net/";
}
