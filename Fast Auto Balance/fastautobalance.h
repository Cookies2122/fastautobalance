#ifndef _INCLUDE_METAMOD_SOURCE_FASTAUTOBALANCE_H_
#define _INCLUDE_METAMOD_SOURCE_FASTAUTOBALANCE_H_

#include <ISmmPlugin.h>
#include <igameevents.h>
#include <iplayerinfo.h>
#include "utlvector.h"
#include "ehandle.h"
#include <sh_vector.h>
#include "iserver.h"
#include "include/menus.h"
#include "include/vip.h"
#include "include/admin.h"
#include "KeyValues.h"
#include "filesystem.h"
#include <map>
#include <string>
#include <vector>
#include <algorithm>

class FastAutoBalance final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
	bool Unload(char* error, size_t maxlen);
	void AllPluginsLoaded();

public:
	const char* GetAuthor();
	const char* GetName();
	const char* GetDescription();
	const char* GetURL();
	const char* GetLicense();
	const char* GetVersion();
	const char* GetDate();
	const char* GetLogTag();
};

extern FastAutoBalance g_FastAutoBalance;

PLUGIN_GLOBALVARS();

#endif