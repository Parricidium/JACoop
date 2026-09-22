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

#include "../client/snd_music.h"	// didn't want to put this in snd_local because of rebuild times etc.
#include "server.h"

#if !defined (MINIHEAP_H_INC)
	#include "../qcommon/MiniHeap.h"
#endif

void CM_CleanLeafCache(void);
extern void SV_FreeClient(client_t*);

CMiniHeap *G2VertSpaceServer = NULL;
/*
Ghoul2 Insert End
*/


/*
===============
SV_SetConfigstring

===============
*/
void SV_SetConfigstring (int index, const char *val) {
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_SetConfigstring: bad index %i\n", index);
	}

	if ( !val ) {
		val = "";
	}

	// don't bother broadcasting an update if no change
	if ( !strcmp( val, sv.configstrings[ index ] ) ) {
		return;
	}

	// change the string in sv
	Z_Free( sv.configstrings[index] );
	sv.configstrings[index] = CopyString( val );

	// send it to all the clients if we aren't
	// spawning a new server
	if ( sv.state == SS_GAME ) {
		client_t	*client;
		int			j;

		for ( j = 0, client = svs.clients; j < MAX_CLIENTS; j++, client++ ) {
			if ( client->state == CS_PRIMED ) {
				// coop: still loading the level: nothing goes out to it before it
				// enters the world, so just remember the index (SV_UpdateConfigstrings)
				if ( !client->csUpdated[index] ) {
					client->csUpdated[index] = qtrue;
					client->csPending++;
				}
				continue;
			}
			if ( client->state < CS_PRIMED ) {
				continue;
			}
			SV_SendServerCommand( client, "cs %i \"%s\"\n", index, val );
		}
	}
}

/*
===============
SV_UpdateConfigstrings

coop: send the configstrings that changed while the client was loading, as
many as the reliable window can take now (SV_SendClientSnapshot calls again
for the rest once the client acknowledges).

Two limits, both needed. The count keeps room in the 64-command window for the
game's own commands, or SV_AddServerCommand drops the connection with "Server
command overflow". The byte budget matters just as much: every snapshot repeats
ALL the unacknowledged commands (SV_UpdateServerCommandsToClient), so a level
whose opening changes dozens of long configstrings can make each message larger
than MAX_MSGLEN -- it overflows, SV_SendClientSnapshot clears it, the client
receives nothing at all, never acknowledges, and the same oversized message is
rebuilt for ever until it times out. On the loopback (the host's own client)
this never shows because the acknowledge comes back the same frame; a joiner one
round trip away is the one that sees it.
===============
*/
#define SV_RELIABLE_BYTE_BUDGET		( MAX_MSGLEN / 2 )	// unacknowledged commands repeated in every snapshot

int SV_ReliableBytesPending( const client_t *client ) {
	int	i, bytes = 0;

	int first = client->reliableSequence - MAX_RELIABLE_COMMANDS + 1;	// coop: never walk outside the ring
	if ( first < client->reliableAcknowledge + 1 ) first = client->reliableAcknowledge + 1;
	if ( first < 0 ) first = 0;
	for ( i = first; i <= client->reliableSequence; i++ ) {
		const char *cmd = client->reliableCommands[ i & ( MAX_RELIABLE_COMMANDS - 1 ) ];
		if ( cmd ) {
			bytes += (int)strlen( cmd ) + 6;	// svc_serverCommand + sequence + terminator
		}
	}
	return bytes;
}

void SV_UpdateConfigstrings( client_t *client ) {
	int	index;
	int	bytes = SV_ReliableBytesPending( client );

	if ( !client->csPending ) {
		return;
	}
	for ( index = 0; index < MAX_CONFIGSTRINGS && client->csPending; index++ ) {
		int	len;

		if ( !client->csUpdated[index] ) {
			continue;
		}
		if ( client->reliableSequence - client->reliableAcknowledge >= MAX_RELIABLE_COMMANDS - 8 ) {
			return;		// keep some room for the game's own commands
		}
		len = (int)strlen( sv.configstrings[index] ) + 20;
		if ( bytes && bytes + len > SV_RELIABLE_BYTE_BUDGET ) {
			return;		// the next snapshot takes the rest, once this much is acknowledged
		}
		bytes += len;
		client->csUpdated[index] = qfalse;
		client->csPending--;
		SV_SendServerCommand( client, "cs %i \"%s\"\n", index, sv.configstrings[index] );
	}
}



/*
===============
SV_GetConfigstring

===============
*/
void SV_GetConfigstring( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetConfigstring: bufferSize == %i", bufferSize );
	}
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_GetConfigstring: bad index %i\n", index);
	}
	if ( !sv.configstrings[index] ) {
		buffer[0] = 0;
		return;
	}

	Q_strncpyz( buffer, sv.configstrings[index], bufferSize );
}


/*
===============
SV_SetUserinfo

===============
*/
void SV_SetUserinfo( int index, const char *val ) {
	if ( index < 0 || index >= MAX_CLIENTS ) {
		Com_Error (ERR_DROP, "SV_SetUserinfo: bad index %i\n", index);
	}

	if ( !val ) {
		val = "";
	}

	Q_strncpyz( svs.clients[ index ].userinfo, val, sizeof( svs.clients[ index ].userinfo ) );
}



/*
===============
SV_GetUserinfo

===============
*/
void SV_GetUserinfo( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetUserinfo: bufferSize == %i", bufferSize );
	}
	if ( index < 0 || index >= MAX_CLIENTS ) {
		Com_Error (ERR_DROP, "SV_GetUserinfo: bad index %i\n", index);
	}
	Q_strncpyz( buffer, svs.clients[ index ].userinfo, bufferSize );
}


/*
================
SV_CreateBaseline

Entity baselines are used to compress non-delta messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
void SV_CreateBaseline( void ) {
	gentity_t			*svent;
	int				entnum;

	for ( entnum = 0; entnum < ge->num_entities ; entnum++ ) {
		svent = SV_GentityNum(entnum);
		if (!svent->inuse) {
			continue;
		}
		if (!svent->linked) {
			continue;
		}
		svent->s.number = entnum;

		//
		// take current state as baseline
		//
		sv.svEntities[entnum].baseline = svent->s;
	}
}




/*
===============
SV_Startup

Called when a game is about to begin
===============
*/
void SV_Startup( void ) {
	if ( svs.initialized ) {
		Com_Error( ERR_FATAL, "SV_Startup: svs.initialized" );
	}

	svs.clients = (struct client_s *) Z_Malloc ( sizeof(client_t) * MAX_CLIENTS, TAG_CLIENTS, qtrue );
	// per-client ring buffer: multiplayer sizes this as
	// sv_maxclients * PACKET_BACKUP * MAX_SNAPSHOT_ENTITIES. The leading
	// factor must scale with the client count or one client's snapshots
	// overwrite another's.
	// coop: it must also be deep enough that a remote client's delta base
	// (up to PACKET_BACKUP frames back) is still in the ring, else every
	// snapshot to it is a full one (see SV_WriteSnapshotToClient).
	svs.numSnapshotEntities = MAX_CLIENTS * PACKET_BACKUP * MAX_SNAPSHOT_ENTITIES;
	svs.initialized = qtrue;

	Cvar_Set( "sv_running", "1" );
}

qboolean CM_SameMap(const char *server);
void Cvar_Defrag(void);

/*
================
SV_SpawnServer

Change the server to a new map, taking all connected
clients along with it.
================
*/
void SV_SpawnServer( const char *server, ForceReload_e eForceReload, qboolean bAllowScreenDissolve )
{
	int			i;
	int			checksum;

	re.RegisterMedia_LevelLoadBegin( server, eForceReload, bAllowScreenDissolve );


	Cvar_SetValue( "cl_paused", 0 );
	Cvar_Set( "timescale", "1" );//jic we were skipping

	// shut down the existing game if it is running
	SV_ShutdownGameProgs(qtrue);

	Com_Printf ("------ Server Initialization ------\n%s\n", com_version->string);
	Com_Printf ("Server: %s\n",server);

	// Moved up from below to help reduce fragmentation
	if (svs.snapshotEntities)
	{
		Z_Free(svs.snapshotEntities);
		svs.snapshotEntities = NULL;
	}

	// don't let sound stutter and dump all stuff on the hunk
	CL_MapLoading();

	if (!CM_SameMap(server))
	{ //rww - only clear if not loading the same map
		CM_ClearMap();
	}

	// Miniheap never changes sizes, so I just put it really early in mem.
	G2VertSpaceServer->ResetHeap();

	Hunk_Clear();

	// wipe the entire per-level structure
	// Also moved up, trying to do all freeing before new allocs
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( sv.configstrings[i] ) {
			Z_Free( sv.configstrings[i] );
			sv.configstrings[i] = NULL;
		}
	}

	// Collect all the small allocations done by the cvar system
	// This frees, then allocates. Make it the last thing before other
	// allocations begin!
	Cvar_Defrag();

/*
		This is useful for debugging memory fragmentation.  Please don't
	   remove it.
*/

	// init client structures and svs.numSnapshotEntities
	// This is moved down quite a bit, but should be safe. And keeps
	// svs.clients right at the beginning of memory
	if ( !Cvar_VariableIntegerValue("sv_running") ) {
		SV_Startup();
	}

 	// clear out those shaders, images and Models
	/*R_InitImages();
	R_InitShaders();
	R_ModelInit();*/

	re.SVModelInit();

	// allocate the snapshot entities
	svs.snapshotEntities = (entityState_t *) Z_Malloc (sizeof(entityState_t)*svs.numSnapshotEntities, TAG_CLIENTS, qtrue );

	Music_SetLevelName(server);

	// toggle the server bit so clients can detect that a
	// server has changed
//!@	svs.snapFlagServerBit ^= SNAPFLAG_SERVERCOUNT;

	// set nextmap to the same map, but it may be overriden
	// by the game startup or another console command
	Cvar_Set( "nextmap", va("map %s", server) );


	memset (&sv, 0, sizeof(sv));


	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		sv.configstrings[i] = CopyString("");
	}

	sv.time = 1000;
	re.G2API_SetTime(sv.time,G2T_SV_TIME);

	CM_LoadMap( va("maps/%s.bsp", server), qfalse, &checksum, qfalse );

	// set serverinfo visible name
	Cvar_Set( "mapname", server );

	Cvar_Set( "sv_mapChecksum", va("%i",checksum) );

	// serverid should be different each time
	sv.serverId = com_frameTime;
	Cvar_Set( "sv_serverid", va("%i", sv.serverId ) );

	// clear physics interaction links
	SV_ClearWorld ();

	// media configstring setting should be done during
	// the loading stage, so connected clients don't have
	// to load during actual gameplay
	sv.state = SS_LOADING;

	// load and spawn all other entities
	SV_InitGameProgs();

	// run a few frames to allow everything to settle
	for ( i = 0 ;i < 4 ; i++ ) {
		ge->RunFrame( sv.time );
		sv.time += 100;
		re.G2API_SetTime(sv.time,G2T_SV_TIME);
	}
#ifndef JK2_MODE
	ge->ConnectNavs(sv_mapname->string, sv_mapChecksum->integer);
#endif

	// create a baseline for more efficient communications
	SV_CreateBaseline ();

	for (i=0 ; i<MAX_CLIENTS ; i++) {
		// clear all time counters, because we have reset sv.time
		svs.clients[i].lastPacketTime = 0;
		svs.clients[i].lastConnectTime = 0;

		// send the new gamestate to all connected clients
		if (svs.clients[i].state >= CS_CONNECTED) {
			char	*denied;

			// connect the client again
			denied = ge->ClientConnect( i, qfalse, eNO/*qfalse*/ );	// firstTime = qfalse, qbFromSavedGame
			if ( denied ) {
				// this generally shouldn't happen, because the client
				// was connected before the level change
				SV_DropClient( &svs.clients[i], denied );
			} else {
				svs.clients[i].state = CS_CONNECTED;
				// when we get the next packet from a connected client,
				// the new gamestate will be sent
			}
		}
	}

	// run another frame to allow things to look at all connected clients
	ge->RunFrame( sv.time );
	sv.time += 100;
	re.G2API_SetTime(sv.time,G2T_SV_TIME);


	// save systeminfo and serverinfo strings
	SV_SetConfigstring( CS_SYSTEMINFO, Cvar_InfoString( CVAR_SYSTEMINFO ) );
	cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;

	SV_SetConfigstring( CS_SERVERINFO, Cvar_InfoString( CVAR_SERVERINFO ) );
	cvar_modifiedFlags &= ~CVAR_SERVERINFO;

	// any media configstring setting now should issue a warning
	// and any configstring changes should be reliably transmitted
	// to all clients
	sv.state = SS_GAME;

	Hunk_SetMark();
	Z_Validate();
	Z_Validate();
	Z_Validate();

	Com_Printf ("-----------------------------------\n");
}

#define G2_VERT_SPACE_SIZE 256
#define G2_MINIHEAP_SIZE	G2_VERT_SPACE_SIZE*1024

/*
===============
SV_Init

Only called at main exe startup, not for each game
===============
*/
void SV_Init (void) {
	SV_AddOperatorCommands ();

	// serverinfo vars
	Cvar_Get ("protocol", va("%i", PROTOCOL_VERSION), CVAR_SERVERINFO | CVAR_ROM);
	sv_mapname = Cvar_Get ("mapname", "nomap", CVAR_SERVERINFO | CVAR_ROM);
	// D2: name shown in the LAN co-op browser. Default to the player's name so a
	// freshly hosted game is identifiable without any configuration.
	sv_hostname = Cvar_Get ("sv_hostname", Cvar_VariableString("name"), CVAR_ARCHIVE);
	// E1: how many players may connect. Latched (fixed for the life of a running
	// server) and clamped to [1, MAX_CLIENTS]; allocations stay MAX_CLIENTS-sized,
	// only the connection-accept loop is bounded by this. Default 2 (co-op pair);
	// coop_host raises it via D1's stored maxplayers.
	sv_maxclients = Cvar_Get ("sv_maxclients", "4", CVAR_SERVERINFO | CVAR_LATCH | CVAR_ARCHIVE);

	// systeminfo
	Cvar_Get ("helpUsObi", "0", CVAR_SYSTEMINFO );
	sv_serverid = Cvar_Get ("sv_serverid", "0", CVAR_SYSTEMINFO | CVAR_ROM );

	// server vars
	sv_fps = Cvar_Get ("sv_fps", "20", CVAR_TEMP );
	sv_timeout = Cvar_Get ("sv_timeout", "120", CVAR_TEMP );
	sv_zombietime = Cvar_Get ("sv_zombietime", "2", CVAR_TEMP );
	Cvar_Get ("nextmap", "", CVAR_TEMP );
	sv_spawntarget = Cvar_Get ("spawntarget", "", 0 );

	sv_reconnectlimit = Cvar_Get ("sv_reconnectlimit", "3", 0);
	sv_showloss = Cvar_Get ("sv_showloss", "0", 0);
	sv_killserver = Cvar_Get ("sv_killserver", "0", 0);
	sv_mapChecksum = Cvar_Get ("sv_mapChecksum", "", CVAR_ROM);
	sv_testsave = Cvar_Get ("sv_testsave", "0", 0);
	sv_compress_saved_games = Cvar_Get ("sv_compress_saved_games", "1", 0);

	SV_CoopTransferInit();	// coop: host -> joiner pk3 transfer cvars

	// Only allocated once, no point in moving it around and fragmenting
	// create a heap for Ghoul2 to use for game side model vertex transforms used in collision detection
	{
		static CMiniHeap singleton(G2_MINIHEAP_SIZE);
		G2VertSpaceServer = &singleton;
	}
}


/*
==================
SV_FinalMessage

Used by SV_Shutdown to send a final message to all
connected clients before the server goes down.  The messages are sent immediately,
not just stuck on the outgoing message list, because the server is going
to totally exit after returning from this function.
==================
*/
void SV_FinalMessage( const char *message ) {
	client_t *cl = svs.clients;

	SV_SendServerCommand( NULL, "print \"%s\"", message );
	SV_SendServerCommand( NULL, "disconnect" );

	// send it twice, ignoring rate
	if ( cl->state >= CS_CONNECTED ) {
		SV_SendClientSnapshot( cl );
		SV_SendClientSnapshot( cl );
	}
}


/*
================
SV_Shutdown

Called when each game quits,
before Sys_Quit or Sys_Error
================
*/
void SV_Shutdown( const char *finalmsg ) {
	int i;

	if ( !com_sv_running || !com_sv_running->integer ) {
		return;
	}

	//Com_Printf( "----- Server Shutdown -----\n" );

	if ( svs.clients && !com_errorEntered ) {
		SV_FinalMessage( finalmsg );
	}

	SV_RemoveOperatorCommands();
	SV_ShutdownGameProgs(qfalse);

	if (svs.snapshotEntities)
	{
		Z_Free(svs.snapshotEntities);
		svs.snapshotEntities = NULL;
	}

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( sv.configstrings[i] ) {
			Z_Free( sv.configstrings[i] );
		}
	}

	// free current level
	memset( &sv, 0, sizeof( sv ) );

	// free server static data
	SV_CoopTransferShutdown();	// coop: close the pak handles of transfers in flight
	if ( svs.clients ) {
		SV_FreeClient(svs.clients);
		Z_Free( svs.clients );
	}
	memset( &svs, 0, sizeof( svs ) );

	// Ensure we free any memory used by the leaf cache.
	CM_CleanLeafCache();

	Cvar_Set( "sv_running", "0" );

	//Com_Printf( "---------------------------\n" );
}

