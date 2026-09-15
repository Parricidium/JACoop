/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "../server/exe_headers.h"

#include "server.h"
#include "../game/weapons.h"
#include "../game/g_items.h"
#include "../game/statindex.h"
#include "../qcommon/game_version.h"

/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/

qboolean qbLoadTransition = qfalse;

//=========================================================
// don't call this directly, it should only be called from SV_Map_f() or SV_MapTransition_f()
//
static bool SV_Map_( ForceReload_e eForceReload )
{
	char		*map = NULL;
	char		expanded[MAX_QPATH] = {0};

	map = Cmd_Argv(1);
	if ( !*map ) {
		Com_Printf ("no map specified\n");
		return false;
	}

	// make sure the level exists before trying to change, so that
	// a typo at the server console won't end the game
	if (strchr (map, '\\') ) {
		Com_Printf ("Can't have mapnames with a \\\n");
		return false;
	}

	Com_sprintf (expanded, sizeof(expanded), "maps/%s.bsp", map);

	if ( FS_ReadFile (expanded, NULL) == -1 ) {
		Com_Printf ("Can't find map %s\n", expanded);
		extern	cvar_t	*com_buildScript;
		if (com_buildScript && com_buildScript->integer)
		{//yes, it's happened, someone deleted a map during my build...
			Com_Error( ERR_FATAL, "Can't find map %s\n", expanded );
		}
		return false;
	}

	if (map[0]!='_')
	{
		SG_WipeSavegame("auto");
	}

	SV_SpawnServer( map, eForceReload, qtrue );	// start up the map
	return true;
}



// co-op: build the per-slot carry-over cvar name. Slot 0 keeps the bare stock
// name (so single-player savegames and the retail restore path are unchanged);
// joiners in slots 1+ get an index suffix (e.g. "playersave1"). psBase is one of
// the four carry-over cvar bases ("playersave"/"playerammo"/"playerinv"/"playerfplvl").
static const char *SV_PlayerSaveCvarName( const char *psBase, int slot )
{
	static char sName[64];
	if ( slot <= 0 )
	{
		return psBase;
	}
	Com_sprintf( sName, sizeof(sName), "%s%i", psBase, slot );
	return sName;
}

// Save out some player data for later restore if this is a spawn point with KEEP_PREV (spawnflags&1) set...
//
// (now also called by auto-save code to setup the cvars correctly
//
// co-op: originally only client 0 (the host) was ever a player. We now loop every
// connected client slot so joiners carry their weapons/health/ammo/force across a
// level transition too; each slot's data lands in its own suffixed cvar set that
// survives the SV_SpawnServer restart (cvars are not hunk-cleared).
void SV_Player_EndOfLevelSave(void)
{
	int	i;
	int	slot;
	const int	maxSlots = Cvar_VariableIntegerValue( "sv_maxclients" );

	for ( slot = 0; slot < maxSlots && slot < MAX_CLIENTS; slot++ )
	{
	client_t* cl = &svs.clients[slot];

	// blank this slot's carry-over so a slot that is not currently a connected
	// player doesn't restore a stale player from an earlier transition.
	Cvar_Set( SV_PlayerSaveCvarName( sCVARNAME_PLAYERSAVE, slot ), "" );
	Cvar_Set( SV_PlayerSaveCvarName( "playerammo", slot ), "" );
	Cvar_Set( SV_PlayerSaveCvarName( "playerinv", slot ), "" );
	Cvar_Set( SV_PlayerSaveCvarName( "playerfplvl", slot ), "" );

	if (cl
		&& cl->state >= CS_CONNECTED	// only slots that are actually connected players
		&&
		cl->gentity && cl->gentity->client	// crash fix for voy4->brig transition when you kill Foster.
											//	Shouldn't happen, but does sometimes...
		)
	{
//		clientSnapshot_t*	pFrame = &cl->frames[cl->netchan.outgoingSequence & PACKET_MASK];
		playerState_t*		pState = cl->gentity->client;
		const char	*s2;
		const char *s;
#ifdef JK2_MODE
		s = va("%i %i %i %i %i %i %i %f %f %f %i %i %i %i %i %i",
						pState->stats[STAT_HEALTH],
						pState->stats[STAT_ARMOR],
						pState->stats[STAT_WEAPONS],
						pState->stats[STAT_ITEMS],
						pState->weapon,
						pState->weaponstate,
						pState->batteryCharge,
						pState->viewangles[0],
						pState->viewangles[1],
						pState->viewangles[2],
						pState->forcePowersKnown,
						pState->forcePower,
						pState->saberActive,
						pState->saberAnimLevel,
						pState->saberLockEnemy,
						pState->saberLockTime
						);
#else
				//				|general info				  |-force powers |-saber 1		|-saber 2										  |-general saber
				s = va("%i %i %i %i %i %i %i %f %f %f %i %i %i %i %i %s %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %s %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i",
						pState->stats[STAT_HEALTH],
						pState->stats[STAT_ARMOR],
						pState->stats[STAT_WEAPONS],
						pState->stats[STAT_ITEMS],
						pState->weapon,
						pState->weaponstate,
						pState->batteryCharge,
						pState->viewangles[0],
						pState->viewangles[1],
						pState->viewangles[2],
						//force power data
						pState->forcePowersKnown,
						pState->forcePower,
						pState->forcePowerMax,
						pState->forcePowerRegenRate,
						pState->forcePowerRegenAmount,
						//saber 1 data
						pState->saber[0].name,
						pState->saber[0].blade[0].active,
						pState->saber[0].blade[1].active,
						pState->saber[0].blade[2].active,
						pState->saber[0].blade[3].active,
						pState->saber[0].blade[4].active,
						pState->saber[0].blade[5].active,
						pState->saber[0].blade[6].active,
						pState->saber[0].blade[7].active,
						pState->saber[0].blade[0].color,
						pState->saber[0].blade[1].color,
						pState->saber[0].blade[2].color,
						pState->saber[0].blade[3].color,
						pState->saber[0].blade[4].color,
						pState->saber[0].blade[5].color,
						pState->saber[0].blade[6].color,
						pState->saber[0].blade[7].color,
						//saber 2 data
						pState->saber[1].name,
						pState->saber[1].blade[0].active,
						pState->saber[1].blade[1].active,
						pState->saber[1].blade[2].active,
						pState->saber[1].blade[3].active,
						pState->saber[1].blade[4].active,
						pState->saber[1].blade[5].active,
						pState->saber[1].blade[6].active,
						pState->saber[1].blade[7].active,
						pState->saber[1].blade[0].color,
						pState->saber[1].blade[1].color,
						pState->saber[1].blade[2].color,
						pState->saber[1].blade[3].color,
						pState->saber[1].blade[4].color,
						pState->saber[1].blade[5].color,
						pState->saber[1].blade[6].color,
						pState->saber[1].blade[7].color,
						//general saber data
						pState->saberStylesKnown,
						pState->saberAnimLevel,
						pState->saberLockEnemy,
						pState->saberLockTime
						);
#endif
		Cvar_Set( SV_PlayerSaveCvarName( sCVARNAME_PLAYERSAVE, slot ), s );

		//ammo
		s2 = "";
		for (i=0;i< AMMO_MAX; i++)
		{
			s2 = va("%s %i",s2, pState->ammo[i]);
		}
		Cvar_Set( SV_PlayerSaveCvarName( "playerammo", slot ), s2 );

		//inventory
		s2 = "";
		for (i=0;i< INV_MAX; i++)
		{
			s2 = va("%s %i",s2, pState->inventory[i]);
		}
		Cvar_Set( SV_PlayerSaveCvarName( "playerinv", slot ), s2 );

		// the new JK2 stuff - force powers, etc...
		//
		s2 = "";
		for (i=0;i< NUM_FORCE_POWERS; i++)
		{
			s2 = va("%s %i",s2, pState->forcePowerLevel[i]);
		}
		Cvar_Set( SV_PlayerSaveCvarName( "playerfplvl", slot ), s2 );
	}
	}	// for slot
}


// Restart the server on a different map
//
static void SV_MapTransition_f(void)
{
	const char	*spawntarget;

#ifdef JK2_MODE
	SCR_PrecacheScreenshot();
#endif
	SV_Player_EndOfLevelSave();

	spawntarget = Cmd_Argv(2);
	if ( *spawntarget != '\0' )
	{
		Cvar_Set( "spawntarget", spawntarget );
	}
	else
	{
		Cvar_Set( "spawntarget", "" );
	}

	SV_Map_( eForceReload_NOTHING );
}

/*
==================
SV_Map_f

Restart the server on a different map, but clears a cvar so that typing "map blah" doesn't try and preserve
player weapons/ammo/etc from the previous level that you haven't really exited (ie ignores KEEP_PREV on spawn points)
==================
*/
#ifdef JK2_MODE
extern void SCR_UnprecacheScreenshot();
#endif
static void SV_Map_f( void )
{
	Cvar_Set( sCVARNAME_PLAYERSAVE, "");
	Cvar_Set( "spawntarget", "" );
	Cvar_Set("tier_storyinfo", "0");
	Cvar_Set("tiers_complete", "");
#ifdef JK2_MODE
	SCR_UnprecacheScreenshot();
#endif

	ForceReload_e eForceReload = eForceReload_NOTHING;	// default for normal load

	char *cmd = Cmd_Argv( 0 );
	if ( !Q_stricmp( cmd, "devmapbsp") )
		eForceReload = eForceReload_BSP;
	else if ( !Q_stricmp( cmd, "devmapmdl") )
		eForceReload = eForceReload_MODELS;
	else if ( !Q_stricmp( cmd, "devmapall") )
		eForceReload = eForceReload_ALL;

	qboolean cheat = (qboolean)(!Q_stricmpn( cmd, "devmap", 6 ) );

	// retain old cheat state
	if ( !cheat && Cvar_VariableIntegerValue( "helpUsObi" ) )
		cheat = qtrue;

	if (SV_Map_( eForceReload ))
	{
		// set the cheat value
		// if the level was started with "map <levelname>", then
		// cheats will not be allowed.  If started with "devmap <levelname>"
		// then cheats will be allowed
		Cvar_Set( "helpUsObi", cheat ? "1" : "0" );
	}
#ifdef JK2_MODE
	Cvar_Set( "cg_missionstatusscreen", "0" );
#endif
}

/*
==================
SV_LoadTransition_f
==================
*/
void SV_LoadTransition_f(void)
{
	const char	*map;
	const char	*spawntarget;

	map = Cmd_Argv(1);
	if ( !*map ) {
		return;
	}

	qbLoadTransition = qtrue;

#ifdef JK2_MODE
	SCR_PrecacheScreenshot();
#endif
	SV_Player_EndOfLevelSave();

	//Save the full current state of the current map so we can return to it later
	SG_WriteSavegame( va("hub/%s", sv_mapname->string), qfalse );

	//set the spawntarget if there is one
	spawntarget = Cmd_Argv(2);
	if ( *spawntarget != '\0' )
	{
		Cvar_Set( "spawntarget", spawntarget );
	}
	else
	{
		Cvar_Set( "spawntarget", "" );
	}

	if ( !SV_TryLoadTransition( map ) )
	{//couldn't load a savegame
		SV_Map_( eForceReload_NOTHING );
	}
	qbLoadTransition = qfalse;
}
//===============================================================

char	*ivtos( const vec3_t v ) {
	static	int		index;
	static	char	str[8][32];
	char	*s;

	// use an array so that multiple vtos won't collide
	s = str[index];
	index = (index + 1)&7;

	Com_sprintf (s, 32, "( %i %i %i )", (int)v[0], (int)v[1], (int)v[2]);

	return s;
}

/*
================
SV_Status_f
================
*/
static void SV_Status_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	cl = &svs.clients[0];

	if ( !cl ) {
		Com_Printf("Server is not running.\n");
		return;
	}

#if defined(_WIN32)
#define STATUS_OS "Windows"
#elif defined(__linux__)
#define STATUS_OS "Linux"
#elif defined(MACOS_X)
#define STATUS_OS "OSX"
#else
#define STATUS_OS "Unknown"
#endif

	Com_Printf( "name    : %s^7\n", cl->name );
	Com_Printf( "score   : %i\n", cl->gentity->client->persistant[PERS_SCORE] );
	Com_Printf( "version : %s %s %i\n", STATUS_OS, JK_VERSION, PROTOCOL_VERSION );
#ifdef JK2_MODE
	Com_Printf( "game    : Jedi Outcast %s\n", FS_GetCurrentGameDir() );
#else
	Com_Printf( "game    : Jedi Academy %s\n", FS_GetCurrentGameDir() );
#endif
	Com_Printf( "map     : %s at %s\n", sv_mapname->string, ivtos( cl->gentity->client->origin ) );
}

/*
===========
SV_Serverinfo_f

Examine the serverinfo string
===========
*/
static void SV_Serverinfo_f( void ) {
	Com_Printf ("Server info settings:\n");
	Info_Print ( Cvar_InfoString( CVAR_SERVERINFO ) );
}


/*
===========
SV_Systeminfo_f

Examine or change the serverinfo string
===========
*/
static void SV_Systeminfo_f( void ) {
	Com_Printf ("System info settings:\n");
	Info_Print ( Cvar_InfoString( CVAR_SYSTEMINFO ) );
}


/*
===========
SV_DumpUser_f

Examine all a users info strings FIXME: move to game
===========
*/
static void SV_DumpUser_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 1 ) {
		Com_Printf ("Usage: info\n");
		return;
	}

	cl = &svs.clients[0];
	if ( !cl->state ) {
		Com_Printf("Client is not active\n");
		return;
	}

	Com_Printf( "userinfo\n" );
	Com_Printf( "--------\n" );
	Info_Print( cl->userinfo );
}

//===========================================================

/*
==================
SV_CompleteMapName
==================
*/
static void SV_CompleteMapName( char *args, int argNum ) {
	if ( argNum == 2 )
		Field_CompleteFilename( "maps", "bsp", qtrue, qfalse );
}

/*
==================
SV_CompleteMapName
==================
*/
static void SV_CompleteSaveName( char *args, int argNum ) {
	if ( argNum == 2 )
		Field_CompleteFilename( "saves", "sav", qtrue, qtrue );
}

/*
==================
SV_AddOperatorCommands
==================
*/
/*
==================
SV_CoopHost_f

coop_host [maxplayers] — start hosting the current singleplayer game over the
network without a restart or any command-line flags. Enables networking and
(re)opens the UDP socket via NET_Restart, then prints the port a second machine
should connect to. maxplayers is accepted and stored for a later task (the
client cap); it does nothing on its own yet.
==================
*/
static int sv_coopMaxPlayers = 0;	// stored for a future max-clients task (E5)

static void SV_CoopHost_f( void ) {
	if ( !com_sv_running->integer ) {
		Com_Printf( "coop_host: no game is running — load a map first.\n" );
		return;
	}

	if ( Cmd_Argc() > 1 ) {
		sv_coopMaxPlayers = atoi( Cmd_Argv( 1 ) );
	}

	// E1: raise the player limit to the requested count (clamped to MAX_CLIENTS).
	// sv_maxclients is CVAR_LATCH; re-Cvar_Get after Cvar_Set applies the latched
	// value so it takes effect for this running server without a map restart.
	if ( sv_coopMaxPlayers > 0 ) {
		int want = sv_coopMaxPlayers;
		if ( want < 1 ) want = 1;
		if ( want > MAX_CLIENTS ) want = MAX_CLIENTS;
		Cvar_Set( "sv_maxclients", va( "%i", want ) );
		sv_maxclients = Cvar_Get( "sv_maxclients", "2", CVAR_SERVERINFO | CVAR_LATCH | CVAR_ARCHIVE );
	}

	// Enable networking and (re)bind the socket for the current cvars. net_enabled
	// is CVAR_LATCH; NET_Restart re-reads it so this takes effect immediately.
	Cvar_Set( "net_enabled", "1" );
	NET_Restart();

	if ( !NET_IsSocketOpen() ) {
		Com_Printf( "coop_host: failed to open a network socket.\n" );
		return;
	}

	// net_port holds the port actually bound (NET_OpenIP scans 29070-29079).
	const int port = Cvar_VariableIntegerValue( "net_port" );
	Com_Printf( "^2Co-op hosting on port %i.\n", port );
	Com_Printf( "A second machine can join with:  connect <this-machine-ip>:%i\n", port );
	if ( sv_coopMaxPlayers > 0 ) {
		Com_Printf( "(max players requested: %i)\n", sv_coopMaxPlayers );
	}
}

void SV_AddOperatorCommands( void ) {
	static qboolean	initialized;

	if ( initialized ) {
		return;
	}
	initialized = qtrue;

	Cmd_AddCommand ("coop_host", SV_CoopHost_f);
	Cmd_AddCommand ("status", SV_Status_f);
	Cmd_AddCommand ("serverinfo", SV_Serverinfo_f);
	Cmd_AddCommand ("systeminfo", SV_Systeminfo_f);
	Cmd_AddCommand ("dumpuser", SV_DumpUser_f);
	Cmd_AddCommand ("sectorlist", SV_SectorList_f);
	Cmd_AddCommand ("map", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "map", SV_CompleteMapName );
	Cmd_AddCommand ("devmap", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "devmap", SV_CompleteMapName );
	Cmd_AddCommand ("devmapbsp", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "devmapbsp", SV_CompleteMapName );
	Cmd_AddCommand ("devmapmdl", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "devmapmdl", SV_CompleteMapName );
	Cmd_AddCommand ("devmapsnd", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "devmapsnd", SV_CompleteMapName );
	Cmd_AddCommand ("devmapall", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "devmapall", SV_CompleteMapName );
	Cmd_AddCommand ("maptransition", SV_MapTransition_f);
	Cmd_AddCommand ("load", SV_LoadGame_f);
	Cmd_SetCommandCompletionFunc( "load", SV_CompleteSaveName );
	Cmd_AddCommand ("loadtransition", SV_LoadTransition_f);
	Cmd_AddCommand ("save", SV_SaveGame_f);
	Cmd_AddCommand ("wipe", SV_WipeGame_f);

//#ifdef _DEBUG
//	extern void UI_Dump_f(void);
//	Cmd_AddCommand ("ui_dump", UI_Dump_f);
//#endif
}

/*
==================
SV_RemoveOperatorCommands
==================
*/
void SV_RemoveOperatorCommands( void ) {
#if 0
	// removing these won't let the server start again
	Cmd_RemoveCommand ("status");
	Cmd_RemoveCommand ("serverinfo");
	Cmd_RemoveCommand ("systeminfo");
	Cmd_RemoveCommand ("dumpuser");
	Cmd_RemoveCommand ("serverrecord");
	Cmd_RemoveCommand ("serverstop");
	Cmd_RemoveCommand ("sectorlist");
#endif
}

