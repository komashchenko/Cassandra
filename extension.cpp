/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Cassandra Extension
 * Copyright (C) 20XX Kyle Sanderson (KyleS)
 * Copyright (C) 2020 Phoenix (˙·٠●Феникс●٠·˙)
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 */

#include "extension.h"
#include <signal.h>
#include <iplayerinfo.h>
#include <inetchannel.h>

Cassandra g_Cassandra;
SMEXT_LINK(&g_Cassandra);

SH_DECL_HOOK6(IServerGameDLL, LevelInit, SH_NOATTRIB, false, bool, const char*, const char*, const char*, const char*, bool, bool);
SH_DECL_HOOK1_void(IServerGameDLL, GameFrame, SH_NOATTRIB, false, bool);

CGlobalVars* g_pGlobals = nullptr;
IPlayerInfoManager* g_pPlayerInfoMngr = nullptr;
IForward* g_pOnServerCrash = nullptr;

static const int installedsignals[] = { SIGALRM, SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
static const size_t installedlen = (sizeof(installedsignals) / sizeof(int));
static struct sigaction savedsig[installedlen];
static int signalmap[63];


bool Cassandra::SDK_OnLoad(char* error, size_t maxlength, bool late)
{
	SH_ADD_HOOK(IServerGameDLL, GameFrame, gamedll, SH_MEMBER(this, &Cassandra::GameFrame), false);
	SH_ADD_HOOK(IServerGameDLL, LevelInit, gamedll, SH_MEMBER(this, &Cassandra::LevelInit), false);
	
	g_pOnServerCrash = forwards->CreateForward("OnServerCrash", ET_Ignore, 0, nullptr);
	
	return true;
}

void Cassandra::SDK_OnUnload()
{
	RemoveSignalHandler();
	
	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, gamedll, SH_MEMBER(this, &Cassandra::GameFrame), false);
	SH_REMOVE_HOOK(IServerGameDLL, LevelInit, gamedll, SH_MEMBER(this, &Cassandra::LevelInit), false);
	
	forwards->ReleaseForward(g_pOnServerCrash);
	
	alarm(0);
}

bool Cassandra::SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetServerFactory, g_pPlayerInfoMngr, IPlayerInfoManager, INTERFACEVERSION_PLAYERINFOMANAGER);
	
	g_pGlobals = ismm->GetCGlobals();

	return true;
}

bool Cassandra::LevelInit(char const* pMapName, char const* pMapEntities, char const* pOldLevel, char const* pLandmarkName, bool loadGame, bool background)
{
	alarm(15); //Level Change, alarm later in-case something happens
	
	RETURN_META_VALUE(MRES_IGNORED, true);
}

void Cassandra::GameFrame(bool simulating)
{
	alarm(10); //Reset our Alarm here. We're still ticking
	
	InstallSignalHandler();
	
	RETURN_META(MRES_IGNORED);
}

void SignalAction(int sig, siginfo_t* pInfo, void* pData)
{
	static bool bUsed = false;
	
	if (bUsed)
	{
		signal(sig, SIG_DFL);
		
		return;
	}
	
	bUsed = true;
	
	alarm(10);
	
	g_Cassandra.Hooped();
	g_Cassandra.RemoveSignalHandler();
	
	signal(sig, savedsig[signalmap[sig]].sa_handler);
	
	//Exit for good measure.
	exit(EXIT_FAILURE);
}

void Cassandra::InstallSignalHandler()
{
	struct sigaction sig;
	unsigned iter;
	
	for (iter = 0; iter < installedlen; iter++)
	{
		sigaction(installedsignals[iter], nullptr, &sig);
		
		if (sig.sa_sigaction != SignalAction)
		{
			break;
		}
	}
	
	if (iter == installedlen)
	{
		return;
	}
	
	sigset_t sigset;
	sigemptyset(&sigset);
	
	for (iter = 0; iter < installedlen; iter++)
	{
		sigaddset(&sigset, installedsignals[iter]);
	}
	
	sig.sa_mask = sigset;
	sig.sa_handler = nullptr;
	sig.sa_sigaction = SignalAction; //We don't really care who gets installed. This is a union on some platforms so we prefer this
	sig.sa_flags = SA_ONSTACK|SA_SIGINFO;
	
	for (iter = 0; iter < installedlen; iter++)
	{
		sigaction(installedsignals[iter], &sig, &savedsig[iter]);
	}
}

void Cassandra::RemoveSignalHandler()
{
	for (unsigned iter = 0; iter < installedlen; iter++)
	{
		sigaction(installedsignals[iter], &savedsig[iter], nullptr);
	}
}

void Cassandra::Hooped()
{
	//Send plugins notification that server is crashing :(
	if (g_pOnServerCrash->GetFunctionCount() > 0)
	{
		g_pOnServerCrash->Execute(nullptr);
	}
	
	//Send retry to clients
	for (int iRetry = 0; iRetry < 3; iRetry++)
	{
		for (int i = 1; i <= g_pGlobals->maxClients; i++)
		{
			edict_t* pEdict = gamehelpers->EdictOfIndex(i);
			
			if (pEdict == nullptr || pEdict->IsFree())
			{
				continue;
			}
			
			IPlayerInfo* pInfo = g_pPlayerInfoMngr->GetPlayerInfo(pEdict);
			INetChannel* pNetChan  = static_cast<INetChannel*>(engine->GetPlayerNetInfo(i));
			
			if (pNetChan  == nullptr || pInfo == nullptr || !pInfo->IsConnected() || pInfo->IsFakeClient())
			{
				continue;
			}
			
			engine->ClientPrintf(pEdict, "[Cassandra] Server Crashed. Recovering...\n");
			engine->ClientCommand(pEdict, "retry");
			
			pNetChan->Transmit();
		}
		
		ThreadSleep(10);
	}
	
	//Unload all plugins
	IPluginIterator* iter = plsys->GetPluginIterator();
	
	while (iter->MorePlugins())
	{
		IPlugin* pPlugin = iter->GetPlugin();
		
		plsys->UnloadPlugin(pPlugin);
		
		iter->NextPlugin();
	}
	
	iter->Release();
}