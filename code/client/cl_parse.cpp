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

// cl_parse.c  -- parse a message received from the server

#include "../server/exe_headers.h"

#include "client.h"
#include "client_ui.h"

const char *svc_strings[256] = {
	"svc_bad",

	"svc_nop",
	"svc_gamestate",
	"svc_configstring",
	"svc_baseline",
	"svc_serverCommand",
	"svc_download",
	"svc_snapshot"
};

void SHOWNET( msg_t *msg, const char *s) {
	if ( cl_shownet->integer >= 2) {
		Com_Printf ("%3i:%s\n", msg->readcount-1, s);
	}
}


/*
=========================================================================

MESSAGE PARSING

=========================================================================
*/

/*
==================
CL_DeltaEntity

Parses deltas from the given base and adds the resulting entity
to the current frame
==================
*/
// Formerly called MSG_ReadEntity, which read an index into the *server's*
// svs.snapshotEntities and dereferenced it -- valid only when client and server
// share a process. MSG_ReadDeltaEntity parses the fields off the wire. It needs
// the entity number, already read by the caller, and the state to delta from:
// the matching entity in the previous frame, or a null state for one newly
// visible, which SV_EmitPacketEntities sends in full. `unchanged` marks an
// entity the server did not write at all; carry the old state forward rather
// than consuming bytes that are not there.
void CL_DeltaEntity (msg_t *msg, clSnapshot_t *frame, int newnum, entityState_t *old,
					 qboolean unchanged)
{
	entityState_t	*state;
	entityState_t	nullEntityState;

	// save the parsed entity state into the big circular buffer so
	// it can be used as the source for a later delta
	state = &cl.parseEntities[cl.parseEntitiesNum & (MAX_PARSE_ENTITIES-1)];

	if ( unchanged ) {
		*state = *old;
	} else {
		if ( !old ) {
			memset( &nullEntityState, 0, sizeof( nullEntityState ) );
			old = &nullEntityState;
		}
		MSG_ReadDeltaEntity( msg, old, state, newnum );
	}

	if ( state->number == (MAX_GENTITIES-1) ) {
		return;		// entity was delta removed
	}
	cl.parseEntitiesNum++;
	frame->numEntities++;
}

/*
==================
CL_ParsePacketEntities

==================
*/
void CL_ParsePacketEntities( msg_t *msg, clSnapshot_t *oldframe, clSnapshot_t *newframe) {
	int			newnum;
	entityState_t	*oldstate;
	int			oldindex, oldnum;

	newframe->parseEntitiesNum = cl.parseEntitiesNum;
	newframe->numEntities = 0;

	// delta from the entities present in oldframe
	oldindex = 0;
	oldstate = NULL;
	if (!oldframe) {
		oldnum = 99999;
	} else {
		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &cl.parseEntities[
				(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
			oldnum = oldstate->number;
		}
	}

	while ( 1 ) {
		// read the entity index number
		newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );

		if ( newnum == (MAX_GENTITIES-1) ) {
			break;
		}

		if ( msg->readcount > msg->cursize ) {
			Com_Error (ERR_DROP,"CL_ParsePacketEntities: end of message");
		}

		while ( oldnum < newnum ) {
			// one or more entities from the old packet are unchanged
			if ( cl_shownet->integer == 3 ) {
				Com_Printf ("%3i:  unchanged: %i\n", msg->readcount, oldnum);
			}
			CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &cl.parseEntities[
					(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
				oldnum = oldstate->number;
			}
		}
		if (oldnum == newnum) {
			// delta from previous state
			if ( cl_shownet->integer == 3 ) {
				Com_Printf ("%3i:  delta: %i\n", msg->readcount, newnum);
			}
			CL_DeltaEntity( msg, newframe, newnum, oldstate, qfalse );

			oldindex++;

			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &cl.parseEntities[
					(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
				oldnum = oldstate->number;
			}
			continue;
		}

		if ( oldnum > newnum ) {
			// delta from baseline
			if ( cl_shownet->integer == 3 ) {
				Com_Printf ("%3i:  baseline: %i\n", msg->readcount, newnum);
			}
			CL_DeltaEntity( msg, newframe, newnum, NULL, qfalse );
			continue;
		}

	}

	// any remaining entities in the old frame are copied over
	while ( oldnum != 99999 ) {
		// one or more entities from the old packet are unchanged
		if ( cl_shownet->integer == 3 ) {
			Com_Printf ("%3i:  unchanged: %i\n", msg->readcount, oldnum);
		}
		CL_DeltaEntity( msg, newframe, oldnum, oldstate, qtrue );

		oldindex++;

		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &cl.parseEntities[
				(oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1)];
			oldnum = oldstate->number;
		}
	}
}


/*
================
CL_ParseSnapshot

If the snapshot is parsed properly, it will be copied to
cl.frame and saved in cl.frames[].  If the snapshot is invalid
for any reason, no changes to the state will be made at all.
================
*/
void CL_ParseSnapshot( msg_t *msg ) {
	int			len;
	clSnapshot_t	*old;
	clSnapshot_t	newSnap;
	int			deltaNum;
	int			oldMessageNum;
	int			i, packetNum;

	// get the reliable sequence acknowledge number
	clc.reliableAcknowledge = MSG_ReadLong( msg );

	// read in the new snapshot to a temporary buffer
	// we will only copy to cl.frame if it is valid
	memset (&newSnap, 0, sizeof(newSnap));

	newSnap.serverCommandNum = clc.serverCommandSequence;

	newSnap.serverTime = MSG_ReadLong( msg );

	// if we were just unpaused, we can only *now* really let the
	// change come into effect or the client hangs.
	cl_paused->modified = qfalse;

	newSnap.messageNum = MSG_ReadLong( msg );
	deltaNum = MSG_ReadByte( msg );
	if ( !deltaNum ) {
		newSnap.deltaNum = -1;
	} else {
		newSnap.deltaNum = newSnap.messageNum - deltaNum;
	}
	newSnap.cmdNum = MSG_ReadLong( msg );
	newSnap.snapFlags = MSG_ReadByte( msg );

	// If the frame is delta compressed from data that we
	// no longer have available, we must suck up the rest of
	// the frame, but not use it, then ask for a non-compressed
	// message
	if ( newSnap.deltaNum <= 0 ) {
		newSnap.valid = qtrue;		// uncompressed frame
		old = NULL;
	} else {
		old = &cl.frames[newSnap.deltaNum & PACKET_MASK];
		if ( !old->valid ) {
			// should never happen
			Com_Printf ("Delta from invalid frame (not supposed to happen!).\n");
		} else if ( old->messageNum != newSnap.deltaNum ) {
			// The frame that the server did the delta from
			// is too old, so we can't reconstruct it properly.
			Com_Printf ("Delta frame too old.\n");
		} else if ( cl.parseEntitiesNum - old->parseEntitiesNum > MAX_PARSE_ENTITIES - 1024 ) {
			// coop: the new frame's entities (up to 1024) must not land on the old frame's
			Com_Printf ("Delta parseEntitiesNum too old.\n");
		} else {
			newSnap.valid = qtrue;	// valid delta parse
		}
	}

	// read areamask
	len = MSG_ReadByte( msg );
	MSG_ReadData( msg, &newSnap.areamask, len);

	// read playerinfo
	SHOWNET( msg, "playerstate" );
	if ( old ) {
		MSG_ReadDeltaPlayerstate( msg, &old->ps, &newSnap.ps );
	} else {
		MSG_ReadDeltaPlayerstate( msg, NULL, &newSnap.ps );
	}

	// read packet entities
	SHOWNET( msg, "packet entities" );
	CL_ParsePacketEntities( msg, old, &newSnap );

	// if not valid, dump the entire thing now that it has
	// been properly read
	if ( !newSnap.valid ) {
		return;
	}

	// clear the valid flags of any snapshots between the last
	// received and this one
	oldMessageNum = cl.frame.messageNum + 1;

	if ( cl.frame.messageNum - oldMessageNum >= PACKET_BACKUP ) {
		oldMessageNum = cl.frame.messageNum - ( PACKET_BACKUP - 1 );
	}
	for ( ; oldMessageNum < newSnap.messageNum ; oldMessageNum++ ) {
		cl.frames[oldMessageNum & PACKET_MASK].valid = qfalse;
	}

	// copy to the current good spot
	cl.frame = newSnap;

	// calculate ping time
	for ( i = 0 ; i < PACKET_BACKUP ; i++ ) {
		packetNum = ( clc.netchan.outgoingSequence - 1 - i ) & PACKET_MASK;
		if ( cl.frame.cmdNum == cl.packetCmdNumber[ packetNum ] ) {
			cl.frame.ping = cls.realtime - cl.packetTime[ packetNum ];
			break;
		}
	}
	// save the frame off in the backup array for later delta comparisons
	cl.frames[cl.frame.messageNum & PACKET_MASK] = cl.frame;

	if (cl_shownet->integer == 3) {
		Com_Printf ("   frame:%i  delta:%i\n", cl.frame.messageNum,
		cl.frame.deltaNum);
	}

	// actions for valid frames
	cl.newSnapshots = qtrue;
}


//=====================================================================

int cl_connectedToCheatServer;

/*
==================
CL_SystemInfoChanged

The systeminfo configstring has been changed, so parse
new information out of it.  This will happen at every
gamestate, and possibly during gameplay.
==================
*/
void CL_SystemInfoChanged( void ) {
	char			*systemInfo;
	const char		*s;
	char			key[MAX_INFO_KEY];
	char			value[MAX_INFO_VALUE];

	systemInfo = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
	cl.serverId = atoi( Info_ValueForKey( systemInfo, "sv_serverid" ) );

	s = Info_ValueForKey( systemInfo, "helpUsObi" );
	cl_connectedToCheatServer = atoi( s );
	if ( !cl_connectedToCheatServer )
	{
		Cvar_SetCheatState();
	}

	// scan through all the variables in the systeminfo and locally set cvars to match
	s = systemInfo;
	while ( s ) {
		Info_NextPair( &s, key, value );
		if ( !key[0] ) {
			break;
		}

		Cvar_Set( key, value );
	}
	//if ( Cvar_VariableIntegerValue("ui_iscensored") == 1 )
	//{
	//	Cvar_Set( "g_dismemberment", "0");
	//}
}

void UI_UpdateConnectionString( const char *string );

/*
==================
CL_ParseGamestate
==================
*/
void CL_ParseGamestate( msg_t *msg ) {
	int				i;
	int				cmd;
	char			*s;

	Con_Close();

	UI_UpdateConnectionString( "" );

	// co-op dual-load: a remote client that is already in a map and receives a
	// fresh gamestate (a level transition / mission reload on the host) must tear
	// down its renderer world and cgame before loading the new one. Otherwise the
	// cgame re-init calls RE_LoadWorldMap while tr.worldMapLoaded is still set from
	// the old map and the renderer aborts with "attempted to redundantly load world
	// map" (tr_bsp.cpp), dropping the joiner. The host reaches this same teardown
	// via CL_MapLoading -> CL_FlushMemory (driven from SV_SpawnServer), which leaves
	// cls.rendererStarted false by the time it parses its own gamestate; so gating
	// on rendererStarted here fires only for the remote client and never double-
	// flushes the host. On the very first connect the renderer isn't started yet,
	// so this is skipped and connect behaves exactly as before.
	if ( cls.rendererStarted ) {
		CL_FlushMemory();
	}

	// wipe local client state
	CL_ClearState();

	// a gamestate always marks a server command sequence
	clc.serverCommandSequence = MSG_ReadLong( msg );

	// parse all the configstrings and baselines
	cl.gameState.dataCount = 1;	// leave a 0 at the beginning for uninitialized configstrings
	while ( 1 ) {
		cmd = MSG_ReadByte( msg );

		if ( cmd <= 0 ) {
			break;
		}

		if ( cmd == svc_configstring ) {
			int		len;

			i = MSG_ReadShort( msg );
			if ( i < 0 || i >= MAX_CONFIGSTRINGS ) {
				Com_Error( ERR_DROP, "configstring > MAX_CONFIGSTRINGS" );
			}
			s = MSG_ReadString( msg );
			len = strlen( s );

			if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
			}

			// append it to the gameState string buffer
			cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
			memcpy( cl.gameState.stringData + cl.gameState.dataCount, s, len + 1 );
			cl.gameState.dataCount += len + 1;
			if ( cl_shownet->integer == 3 ) {
				Com_Printf ("%3i:  CS# %d %s (%d)\n",msg->readcount, i,s,len);
			}
		} else if ( cmd == svc_baseline ) {
			assert(0);
		} else {
			Com_Error( ERR_DROP, "CL_ParseGamestate: bad command byte" );
		}
	}

	// parse serverId and other cvars
	CL_SystemInfoChanged();

	// reinitialize the filesystem if the game directory has changed
#if 0
	if ( fs_game->modified ) {
	}
#endif

	// let the client game init and load data
	cls.state = CA_LOADING;

	CL_StartHunkUsers();

	// make sure the game starts
	Cvar_Set( "cl_paused", "0" );

	// coop: a joiner asks the host for the player-model pk3s it lacks (once per connection)
	CL_CoopTransferHello();
}


//=====================================================================

void CL_FreeServerCommands(void)
{
	int i;

	for(i=0; i<MAX_RELIABLE_COMMANDS; i++) {
		if ( clc.serverCommands[ i ] ) {
			Z_Free( clc.serverCommands[ i ] );
			clc.serverCommands[ i ] = NULL;
		}
	}
}


/*
=====================
CL_ParseCommandString

Command strings are just saved off until cgame asks for them
when it transitions a snapshot
=====================
*/
void CL_ParseCommandString( msg_t *msg ) {
	char	*s;
	int		seq;
	int		index;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed stored it off
	if ( clc.serverCommandSequence >= seq ) {
		return;
	}
	clc.serverCommandSequence = seq;

	index = seq & (MAX_RELIABLE_COMMANDS-1);
	if ( clc.serverCommands[ index ] ) {
		Z_Free( clc.serverCommands[ index ] );
	}
	clc.serverCommands[ index ] = CopyString( s );

	// coop: the pk3 transfer commands are the engine's, run at receive time
	// (the cgame may not be up yet, and CL_GetServerCommand hides them from it).
	// SV_SendServerCommand leaves its opcode byte at the front of the string:
	// skip it like Cmd_TokenizeString does.
	{
		const char *cmd = s;
		while ( *cmd && *(const unsigned char *)cmd <= ' ' ) {
			cmd++;
		}
		if ( !Q_strncmp( cmd, "coopdl_", 7 ) || !Q_strncmp( cmd, "coopup_", 7 ) ) {
			CL_CoopTransferServerCommand( cmd );
		}
		// "vid" alone = the host's in-game video is over (it may have skipped it): ours ends now,
		// the cgame cannot do it since it does not run while the video plays
		if ( !com_sv_running->integer && !Q_strncmp( cmd, "vid", 3 ) && ( cmd[3] == '\0' || cmd[3] == ' ' ) ) {
			const char *arg = cmd + 3;
			while ( *arg == ' ' ) arg++;
			if ( !*arg && CL_IsRunningInGameCinematic() ) {
				SCR_StopCinematic();
			}
		}
	}
}


/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseServerMessage( msg_t *msg ) {
	int			cmd;

	if ( cl_shownet->integer == 1 ) {
		Com_Printf ("%i ",msg->cursize);
	} else if ( cl_shownet->integer >= 2 ) {
		Com_Printf ("------------------\n");
	}

	//
	// parse the message
	//
	while ( 1 ) {
		if ( msg->readcount > msg->cursize ) {
			Com_Error (ERR_DROP,"CL_ParseServerMessage: read past end of server message");
			break;
		}

		cmd = MSG_ReadByte( msg );

		if ( cmd == -1 ) {
			SHOWNET( msg, "END OF MESSAGE" );
			break;
		}

		if ( cl_shownet->integer >= 2 ) {
			if ( !svc_strings[cmd] ) {
				Com_Printf( "%3i:BAD CMD %i\n", msg->readcount-1, cmd );
			} else {
				SHOWNET( msg, svc_strings[cmd] );
			}
		}

	// other commands
		switch ( cmd ) {
		default:
			Com_Error (ERR_DROP,"CL_ParseServerMessage: Illegible server message\n");
			break;
		case svc_nop:
			break;
		case svc_serverCommand:
			CL_ParseCommandString( msg );
			break;
		case svc_gamestate:
			CL_ParseGamestate( msg );
			break;
		case svc_snapshot:
			CL_ParseSnapshot( msg );
			break;
		case svc_download:	// coop: a block of a pk3 the host sends us
			CL_ParseDownload( msg );
			break;
		}
	}

	CL_CoopTransferEndOfMessage();	// coop: one ack per message that carried blocks
}


