#include "fastautobalance.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

FastAutoBalance g_FastAutoBalance;
PLUGIN_EXPOSE(FastAutoBalance, g_FastAutoBalance);
PLUGIN_GLOBALVARS();

IUtilsApi*       g_pUtils       = nullptr;
IPlayersApi*     g_pPlayers     = nullptr;
IVIPApi*         g_pVIPCore     = nullptr;
IAdminApi*       g_pAdminCore   = nullptr;
IVEngineServer2* engine         = nullptr;
IFileSystem*     filesystem     = nullptr;

CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem*     g_pEntitySystem     = nullptr;
CGlobalVars*       gpGlobals           = nullptr;

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils ? g_pUtils->GetCGameEntitySystem() : nullptr;
}

namespace
{
	constexpr int MAX_SLOTS = 64;
	constexpr int T_SPEC    = 1;
	constexpr int T_T       = 2;
	constexpr int T_CT      = 3;
	constexpr const char* TAG = "[FAB]";
	constexpr const char* VER = "2.1.0";

	struct Cfg
	{
		int  dieGap         = 2;
		int  swapGap        = 1;
		bool yapAtPlayer    = true;
		bool noisy          = false;
		bool muteNative     = true;

		bool admShield      = true;
		std::string admPerm = "@admin/balance";
		int  admDieGap      = 3;
		int  admSwapGap     = 2;

		bool vipShield      = true;
		std::vector<std::string> vipBands;
		int  vipDieGap      = 3;
		int  vipSwapGap     = 2;
	};

	Cfg cfg;

	int  slotTeam[MAX_SLOTS]  = {0};
	bool selfNudge[MAX_SLOTS] = {false};
	bool vetoMark[MAX_SLOTS]  = {false};

	struct Marked
	{
		int dst;
		int src;
		int rno;
	};

	std::map<int, Marked> queue;
	int rno = 0;

	std::map<std::string, std::string> loc;
}

static bool slotOk(int s) { return s >= 0 && s < MAX_SLOTS; }

static bool realDude(int s)
{
	return g_pPlayers
		&& g_pPlayers->IsConnected(s)
		&& !g_pPlayers->IsFakeClient(s);
}

static const char* sideName(int t)
{
	if (t == T_T)    return "T";
	if (t == T_CT)   return "CT";
	if (t == T_SPEC) return "SPEC";
	return "NONE";
}

static const char* nameOf(int s)
{
	const char* n = g_pPlayers ? g_pPlayers->GetPlayerName(s) : nullptr;
	return (n && n[0]) ? n : "Unknown";
}

static void mkLogDir()
{
	if (filesystem) filesystem->CreateDirHierarchy("addons/logs/fab", "GAME");
}

static void dbg(const char* fmt, ...)
{
	if (!cfg.noisy) return;

	mkLogDir();

	time_t raw = time(nullptr);
	struct tm tmNow;
#ifdef _WIN32
	localtime_s(&tmNow, &raw);
#else
	localtime_r(&raw, &tmNow);
#endif

	char d[32];
	snprintf(d, sizeof(d), "%02d_%02d_%04d",
		tmNow.tm_mday, tmNow.tm_mon + 1, tmNow.tm_year + 1900);

	char p[512];
	g_SMAPI->PathFormat(p, sizeof(p),
		"%s/addons/logs/fab/fab_%s.txt", g_SMAPI->GetBaseDir(), d);

	FILE* fp = fopen(p, "a");
	if (!fp) return;

	char tStr[16];
	snprintf(tStr, sizeof(tStr), "%02d:%02d:%02d",
		tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

	char buf[1024];
	va_list a;
	va_start(a, fmt);
	vsnprintf(buf, sizeof(buf), fmt, a);
	va_end(a);

	fprintf(fp, "[%s] %s\n", tStr, buf);
	fclose(fp);
}

static std::vector<std::string> chopCsv(const std::string& s)
{
	std::vector<std::string> out;
	size_t start = 0;
	for (size_t i = 0; i <= s.length(); ++i)
	{
		if (i == s.length() || s[i] == ',')
		{
			size_t b = start, e = i;
			while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
			while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
			if (b < e) out.emplace_back(s.substr(b, e - b));
			start = i + 1;
		}
	}
	return out;
}

static void headCount(int& t, int& ct, int skip = -1)
{
	t = 0;
	ct = 0;
	for (int i = 0; i < MAX_SLOTS; ++i)
	{
		if (i == skip)              continue;
		if (!realDude(i))           continue;
		if (slotTeam[i] == T_T)     ++t;
		else if (slotTeam[i] == T_CT) ++ct;
	}
}

static bool isAdmin(int s)
{
	if (!cfg.admShield || !g_pAdminCore) return false;
	return g_pAdminCore->HasPermission(s, cfg.admPerm.c_str());
}

static bool isVip(int s)
{
	if (!cfg.vipShield || !g_pVIPCore) return false;
	if (!g_pVIPCore->VIP_IsClientVIP(s)) return false;
	if (cfg.vipBands.empty()) return true;

	const char* g = g_pVIPCore->VIP_GetClientVIPGroup(s);
	if (!g || !g[0]) return false;
	for (const auto& v : cfg.vipBands)
		if (v == g) return true;
	return false;
}

static int dieLimit(int s)
{
	if (isAdmin(s)) return cfg.admDieGap;
	if (isVip(s))   return cfg.vipDieGap;
	return cfg.dieGap;
}

static int swapLimit(int s)
{
	if (isAdmin(s)) return cfg.admSwapGap;
	if (isVip(s))   return cfg.vipSwapGap;
	return cfg.swapGap;
}

static const char* tier(int s)
{
	if (isAdmin(s)) return "Admin";
	if (isVip(s))   return "VIP";
	return "Player";
}

static const char* phrase(const char* k)
{
	auto it = loc.find(k);
	if (it != loc.end()) return it->second.c_str();

	if (!strcmp(k, "FAB_Chat_T"))
		return "{BLUE}[FAB] {DEFAULT}You were transferred to the team {RED}Terrorists {DEFAULT}for balance";
	if (!strcmp(k, "FAB_Chat_CT"))
		return "{BLUE}[FAB] {DEFAULT}You were transferred to the team {RED}Counter-Terrorists {DEFAULT}for balance";
	if (!strcmp(k, "FAB_Block"))
		return "{BLUE}[FAB] {DEFAULT}You cannot switch to this team, the difference is too big!";
	return k;
}

static void lazyMove(int s, int dst)
{
	if (!g_pUtils || !g_pPlayers) return;

	g_pUtils->CreateTimer(0.1f, [s, dst]() -> float {
		if (!g_pPlayers || !g_pPlayers->IsConnected(s))
			return -1.0f;
		selfNudge[s] = true;
		g_pPlayers->SwitchTeam(s, dst);
		return -1.0f;
	});
}

static void killNative()
{
	if (!engine) return;
	if (!cfg.muteNative)
	{
		dbg("[NATIVE] muteNative=0 -- not touching mp_autoteambalance / mp_limitteams");
		return;
	}
	engine->ServerCommand("mp_autoteambalance 0\n");
	engine->ServerCommand("mp_limitteams 0\n");
	Msg("%s native balance disabled (mp_autoteambalance 0, mp_limitteams 0)\n", TAG);
	dbg("[NATIVE] forced mp_autoteambalance 0 + mp_limitteams 0");
}

static void pullCfg()
{
	cfg = Cfg();

	KeyValues* kv = new KeyValues("fab");
	const char* path = "addons/configs/fastautobalance.ini";

	if (!kv->LoadFromFile(filesystem, path, "GAME"))
	{
		Msg("%s no cfg at %s, using defaults\n", TAG, path);
		delete kv;
		return;
	}

	cfg.dieGap       = kv->GetInt("MaxAD",            cfg.dieGap);
	cfg.swapGap      = kv->GetInt("block",            cfg.swapGap);
	cfg.yapAtPlayer  = kv->GetBool("msg",             cfg.yapAtPlayer);
	cfg.noisy        = kv->GetBool("debug",           cfg.noisy);
	cfg.muteNative   = kv->GetBool("force_native_off", cfg.muteNative);

	cfg.admShield    = kv->GetBool("admin_imune",     cfg.admShield);
	cfg.admPerm      = kv->GetString("admin_flags",   cfg.admPerm.c_str());
	cfg.admDieGap    = kv->GetInt("admin_max",        cfg.admDieGap);
	cfg.admSwapGap   = kv->GetInt("admin_block",      cfg.admSwapGap);

	cfg.vipShield    = kv->GetBool("vip_imune",       cfg.vipShield);
	cfg.vipDieGap    = kv->GetInt("vip_max",          cfg.vipDieGap);
	cfg.vipSwapGap   = kv->GetInt("vip_block",        cfg.vipSwapGap);

	cfg.vipBands.clear();
	const char* vg = kv->GetString("vip_groups", "");
	if (vg && *vg) cfg.vipBands = chopCsv(vg);

	delete kv;

	Msg("%s cfg: die=%d swap=%d msg=%d dbg=%d native_off=%d | adm: shield=%d die=%d swap=%d perm=%s | vip: shield=%d die=%d swap=%d\n",
		TAG,
		cfg.dieGap, cfg.swapGap, (int)cfg.yapAtPlayer, (int)cfg.noisy, (int)cfg.muteNative,
		(int)cfg.admShield, cfg.admDieGap, cfg.admSwapGap, cfg.admPerm.c_str(),
		(int)cfg.vipShield, cfg.vipDieGap, cfg.vipSwapGap);

	if (cfg.noisy)
	{
		std::string vs = "all";
		if (!cfg.vipBands.empty())
		{
			vs.clear();
			for (size_t i = 0; i < cfg.vipBands.size(); ++i)
			{
				if (i) vs += ",";
				vs += cfg.vipBands[i];
			}
		}
		dbg("[CFG] die=%d swap=%d msg=%d native_off=%d | adm: shield=%d die=%d swap=%d perm=%s | vip: shield=%d die=%d swap=%d bands=%s",
			cfg.dieGap, cfg.swapGap, (int)cfg.yapAtPlayer, (int)cfg.muteNative,
			(int)cfg.admShield, cfg.admDieGap, cfg.admSwapGap, cfg.admPerm.c_str(),
			(int)cfg.vipShield, cfg.vipDieGap, cfg.vipSwapGap,
			vs.c_str());
	}
}

static void pullPhrases()
{
	loc.clear();

	KeyValues* kv = new KeyValues("Phrases");
	const char* path = "addons/translations/fab_phrases.txt";

	if (!kv->LoadFromFile(filesystem, path, "GAME"))
	{
		Msg("%s no phrases at %s, using defaults\n", TAG, path);
		delete kv;
		return;
	}

	const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "en";

	FOR_EACH_SUBKEY(kv, p)
	{
		const char* k = p->GetName();
		const char* v = p->GetString(lang, "");
		if (k && v && v[0]) loc[k] = v;
	}

	delete kv;
	Msg("%s phrases for: %s\n", TAG, lang ? lang : "en");
}

static bool tagDeath(int s, int t, int ct, int side)
{
	const int lim = dieLimit(s);

	if (side == T_T && t > ct && (t - ct) > lim)
	{
		if (t <= 0)
		{
			dbg("[DEATH] slot %d (%s) died T | T=%d CT=%d | safety: empty T | NOT marked",
				s, nameOf(s), t, ct);
			return false;
		}
		queue[s] = { T_CT, T_T, rno };
		dbg("[DEATH] slot %d (%s) %s died T | T=%d CT=%d gap=%d > lim=%d | MARKED for CT (round %d)",
			s, nameOf(s), tier(s), t, ct, (t - ct), lim, rno);
		Msg("%s marked %d: T->CT (round %d)\n", TAG, s, rno);
		return true;
	}

	if (side == T_CT && ct > t && (ct - t) > lim)
	{
		if (ct <= 0)
		{
			dbg("[DEATH] slot %d (%s) died CT | T=%d CT=%d | safety: empty CT | NOT marked",
				s, nameOf(s), t, ct);
			return false;
		}
		queue[s] = { T_T, T_CT, rno };
		dbg("[DEATH] slot %d (%s) %s died CT | T=%d CT=%d gap=%d > lim=%d | MARKED for T (round %d)",
			s, nameOf(s), tier(s), t, ct, (ct - t), lim, rno);
		Msg("%s marked %d: CT->T (round %d)\n", TAG, s, rno);
		return true;
	}

	dbg("[DEATH] slot %d (%s) died %s | T=%d CT=%d gap=%d <= lim=%d | NOT marked",
		s, nameOf(s), sideName(side), t, ct, abs(t - ct), lim);
	return false;
}

static void gateSwap(const char*, IGameEvent* e, bool)
{
	if (!e || !g_pPlayers || !g_pUtils) return;

	const int  s  = e->GetInt("userid");
	const int  nt = e->GetInt("team");
	const int  ot = e->GetInt("oldteam");
	const bool dc = e->GetBool("disconnect");

	if (!slotOk(s))                  return;
	if (g_pPlayers->IsFakeClient(s)) return;
	if (dc)                          return;
	if (nt <= T_SPEC)                return;
	if (ot <= T_SPEC)                return;
	if (nt == ot)                    return;

	if (selfNudge[s])
	{
		selfNudge[s] = false;
		return;
	}

	int t, ct;
	headCount(t, ct, s);
	if (nt == T_T)       ++t;
	else if (nt == T_CT) ++ct;

	const int gap = abs(t - ct);
	const int lim = swapLimit(s);

	if (gap > lim)
	{
		vetoMark[s] = true;
		lazyMove(s, ot);

		dbg("[VETO] slot %d (%s) %s tried %s->%s | would be T=%d CT=%d gap=%d > lim=%d | BLOCKED",
			s, nameOf(s), tier(s), sideName(ot), sideName(nt), t, ct, gap, lim);

		if (cfg.yapAtPlayer)
			g_pUtils->PrintToChat(s, " %s", phrase("FAB_Block"));
	}
	else
	{
		dbg("[OK]   slot %d (%s) %s switch %s->%s | will be T=%d CT=%d gap=%d <= lim=%d | ALLOW",
			s, nameOf(s), tier(s), sideName(ot), sideName(nt), t, ct, gap, lim);
	}
}

static void noteSwap(const char*, IGameEvent* e, bool)
{
	if (!e) return;
	const int s  = e->GetInt("userid");
	const int nt = e->GetInt("team");
	const int ot = e->GetInt("oldteam");
	if (!slotOk(s)) return;

	if (vetoMark[s])
	{
		vetoMark[s] = false;
		dbg("[TEAM] slot %d (%s) %d->%d | (vetoed, skip)",
			s, nameOf(s), ot, nt);
		return;
	}

	const int prev = slotTeam[s];
	slotTeam[s] = nt;

	auto it = queue.find(s);
	if (it != queue.end())
	{
		const bool gone =
			(nt <= T_SPEC) ||
			(nt != it->second.src);
		if (gone)
		{
			dbg("[TEAM] slot %d (%s) %s->%s | was queued->%s | DROPPED",
				s, nameOf(s), sideName(ot), sideName(nt),
				sideName(it->second.dst));
			queue.erase(it);
		}
	}

	if (cfg.noisy)
	{
		int t, ct;
		headCount(t, ct);
		dbg("[TEAM] slot %d (%s) %s->%s | now T=%d CT=%d (cached %d)",
			s, nameOf(s), sideName(ot), sideName(nt), t, ct, prev);
	}
}

static void onDeath(const char*, IGameEvent* e, bool)
{
	if (!e) return;

	const int s = e->GetInt("userid");
	if (!slotOk(s))    return;
	if (!realDude(s))  return;

	const int side = slotTeam[s];
	if (side != T_T && side != T_CT) return;

	int t, ct;
	headCount(t, ct);
	if (side == T_T)       --t;
	else                   --ct;

	tagDeath(s, t, ct, side);
}

static void wipeStaleMark(const char*, IGameEvent* e, bool)
{
	if (!e) return;
	const int s = e->GetInt("userid");
	if (!slotOk(s)) return;

	auto it = queue.find(s);
	if (it != queue.end())
	{
		dbg("[SPAWN] slot %d (%s) | still queued->%s on spawn | DROPPED",
			s, nameOf(s), sideName(it->second.dst));
		queue.erase(it);
	}
}

static void flushQueue(const char*, IGameEvent*, bool)
{
	++rno;

	int t, ct;
	headCount(t, ct);
	const int q = (int)queue.size();

	dbg("[ROUND] %d started | T=%d CT=%d gap=%d | q=%d", rno, t, ct, abs(t - ct), q);
	Msg("%s round %d started\n", TAG, rno);

	if (q == 0) return;

	std::vector<int> ids;
	ids.reserve(queue.size());
	for (const auto& kv : queue) ids.push_back(kv.first);

	for (int s : ids)
	{
		auto it = queue.find(s);
		if (it == queue.end()) continue;

		const Marked m = it->second;

		if (!realDude(s))
		{
			dbg("[ROUND] slot %d (%s) | not active | DROPPED", s, nameOf(s));
			queue.erase(s);
			continue;
		}

		const int cur = slotTeam[s];
		if (cur != m.src)
		{
			dbg("[ROUND] slot %d (%s) | side moved (now %s, died %s) | DROPPED",
				s, nameOf(s), sideName(cur), sideName(m.src));
			queue.erase(s);
			continue;
		}

		int tn, ctn;
		headCount(tn, ctn);

		const int lim = dieLimit(s);
		const char* tt = tier(s);

		const bool dropToCT = (m.dst == T_CT && tn > ctn && (tn - ctn) > lim);
		const bool dropToT  = (m.dst == T_T  && ctn > tn && (ctn - tn) > lim);

		if (!dropToCT && !dropToT)
		{
			dbg("[ROUND] slot %d (%s) %s | T=%d CT=%d gap=%d <= lim=%d | not needed | DROPPED",
				s, nameOf(s), tt, tn, ctn, abs(tn - ctn), lim);
			queue.erase(s);
			continue;
		}

		const int ta = (m.dst == T_T)  ? tn + 1  : tn - 1;
		const int ca = (m.dst == T_CT) ? ctn + 1 : ctn - 1;

		if (ta <= 0 || ca <= 0)
		{
			dbg("[ROUND] slot %d (%s) %s | safety: empty side after (T=%d CT=%d) | DROPPED",
				s, nameOf(s), tt, ta, ca);
			queue.erase(s);
			continue;
		}

		const int gn = abs(tn - ctn);
		const int ga = abs(ta - ca);
		if (ga >= gn)
		{
			dbg("[ROUND] slot %d (%s) %s | safety: gap stays (now=%d after=%d) | DROPPED",
				s, nameOf(s), tt, gn, ga);
			queue.erase(s);
			continue;
		}

		lazyMove(s, m.dst);
		slotTeam[s] = m.dst;

		dbg("[ROUND] slot %d (%s) %s | T=%d CT=%d gap=%d > lim=%d | FLIPPED %s->%s",
			s, nameOf(s), tt, tn, ctn, gn, lim, sideName(m.src), sideName(m.dst));
		Msg("%s flipped %d -> team %d (T=%d CT=%d)\n", TAG, s, m.dst, tn, ctn);

		if (cfg.yapAtPlayer && g_pUtils)
		{
			const char* msg = (m.dst == T_CT)
				? phrase("FAB_Chat_CT")
				: phrase("FAB_Chat_T");
			g_pUtils->PrintToChat(s, " %s", msg);
		}

		queue.erase(s);
	}
}

static void wireSlot(const char*, IGameEvent* e, bool)
{
	if (!e) return;
	const int s = e->GetInt("userid");
	if (!slotOk(s)) return;

	slotTeam[s]  = 0;
	selfNudge[s] = false;
	vetoMark[s]  = false;
}

static void unwireSlot(const char*, IGameEvent* e, bool)
{
	if (!e) return;
	const int s = e->GetInt("userid");
	if (!slotOk(s)) return;

	auto it = queue.find(s);
	if (it != queue.end())
	{
		dbg("[DC] slot %d (%s) | was queued->%s | DROPPED",
			s, nameOf(s), sideName(it->second.dst));
		queue.erase(it);
		Msg("%s dropped %d: dc\n", TAG, s);
	}

	slotTeam[s]  = 0;
	selfNudge[s] = false;
	vetoMark[s]  = false;
}

static bool reloadCmd(int s, const char*)
{
	pullCfg();
	pullPhrases();
	killNative();

	dbg("[RELOAD] reloaded by slot %d (%s)", s, nameOf(s));

	if (g_pUtils)
		g_pUtils->PrintToChat(s, " \x0B%s \x04reloaded", TAG);
	return true;
}

static void bootHooks()
{
	g_pGameEntitySystem = g_pUtils ? g_pUtils->GetCGameEntitySystem() : nullptr;
	g_pEntitySystem     = g_pUtils ? g_pUtils->GetCEntitySystem()     : nullptr;
	gpGlobals           = g_pUtils ? g_pUtils->GetCGlobalVars()       : nullptr;

	pullCfg();
	pullPhrases();
	killNative();

	rno = 0;
	queue.clear();
	for (int i = 0; i < MAX_SLOTS; ++i)
	{
		slotTeam[i]  = 0;
		selfNudge[i] = false;
		vetoMark[i]  = false;
	}

	if (cfg.noisy)
	{
		dbg("========================================");
		dbg("[BOOT] FastAutoBalance v%s up", VER);
		dbg("========================================");
	}
}

bool FastAutoBalance::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory,     engine,     IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, filesystem, IFileSystem,     FILESYSTEM_INTERFACE_VERSION);

	g_SMAPI->AddListener(this, this);

	Msg("%s v%s loading...\n", TAG, VER);
	return true;
}

bool FastAutoBalance::Unload(char*, size_t)
{
	if (g_pUtils) g_pUtils->ClearAllHooks(g_PLID);

	loc.clear();
	cfg.vipBands.clear();
	queue.clear();

	Msg("%s bye\n", TAG);
	return true;
}

void FastAutoBalance::AllPluginsLoaded()
{
	Msg("%s AllPluginsLoaded() v%s\n", TAG, VER);

	int ret;

	g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED || !g_pUtils)
	{
		Msg("%s ERR: Utils API missing\n", TAG);
		return;
	}

	g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED || !g_pPlayers)
	{
		Msg("%s ERR: Players API missing\n", TAG);
		return;
	}

	g_pVIPCore = (IVIPApi*)g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED) { g_pVIPCore = nullptr; Msg("%s VIP API skipped\n", TAG); }

	g_pAdminCore = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED) { g_pAdminCore = nullptr; Msg("%s Admin API skipped\n", TAG); }

	for (int i = 0; i < MAX_SLOTS; ++i)
	{
		slotTeam[i]  = 0;
		selfNudge[i] = false;
		vetoMark[i]  = false;
	}

	g_pUtils->StartupServer(g_PLID, bootHooks);

	Msg("%s wiring events...\n", TAG);
	g_pUtils->HookEvent(g_PLID, "player_team",         gateSwap);
	g_pUtils->HookEvent(g_PLID, "player_team",         noteSwap);
	g_pUtils->HookEvent(g_PLID, "player_death",        onDeath);
	g_pUtils->HookEvent(g_PLID, "player_spawn",        wipeStaleMark);
	g_pUtils->HookEvent(g_PLID, "round_start",         flushQueue);
	g_pUtils->HookEvent(g_PLID, "player_connect_full", wireSlot);
	g_pUtils->HookEvent(g_PLID, "player_disconnect",   unwireSlot);

	g_pUtils->RegCommand(g_PLID, { "fab_reload" }, {}, reloadCmd);

	Msg("%s ready!\n", TAG);
}

/////////////////////////////////////////////////////////////////
const char *FastAutoBalance::GetLicense()
{
	return "Public";
}

const char *FastAutoBalance::GetVersion()
{
	return "1.4.0";
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
