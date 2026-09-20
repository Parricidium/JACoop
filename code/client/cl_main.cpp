/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
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

// cl_main.c  -- client main loop

#include "../server/exe_headers.h"

#include <time.h>
#include "client.h"
#include "client_ui.h"
#include <limits.h>
#include "../ghoul2/G2.h"
#include "qcommon/stringed_ingame.h"
#include "sys/sys_loadlib.h"
#include "qcommon/ojk_saved_game.h"

#define	RETRANSMIT_TIMEOUT	3000	// time between connection packet retransmits

cvar_t	*cl_renderer;

cvar_t	*cl_nodelta;
cvar_t	*cl_debugMove;

cvar_t	*cl_noprint;

cvar_t	*cl_timeout;
cvar_t	*cl_packetdup;
cvar_t	*cl_timeNudge;
cvar_t	*cl_showTimeDelta;
cvar_t	*cl_newClock=0;

cvar_t	*cl_shownet;
cvar_t	*cl_avidemo;

cvar_t	*cl_pano;
cvar_t	*cl_panoNumShots;
cvar_t	*cl_skippingcin;
cvar_t	*cl_endcredits;

cvar_t	*cl_freelook;
cvar_t	*cl_sensitivity;

cvar_t	*cl_mouseAccel;
cvar_t	*cl_showMouseRate;
cvar_t	*cl_framerate;

cvar_t	*m_pitch;
cvar_t	*m_yaw;
cvar_t	*m_forward;
cvar_t	*m_side;
cvar_t	*m_filter;

cvar_t	*cl_activeAction;

cvar_t	*cl_allowAltEnter;

cvar_t	*cl_inGameVideo;

cvar_t	*cl_consoleKeys;
cvar_t	*cl_consoleUseScanCode;
cvar_t	*cl_consoleShiftRequirement;

clientActive_t		cl;
clientConnection_t	clc;
clientStatic_t		cls;

// Structure containing functions exported from refresh DLL
refexport_t	re;
static void *rendererLib = NULL;

//RAZFIXME: BAD BAD, maybe? had to move it out of ghoul2_shared.h -> CGhoul2Info_v at the least..
IGhoul2InfoArray &_TheGhoul2InfoArray( void ) {
	return re.TheGhoul2InfoArray();
}

static void CL_ShutdownRef( qboolean restarting );
void CL_InitRef( void );
void CL_CheckForResend( void );

/*
=======================================================================

CLIENT RELIABLE COMMAND COMMUNICATION

=======================================================================
*/

/*
======================
CL_AddReliableCommand

The given command will be transmitted to the server, and is gauranteed to
not have future usercmd_t executed before it is executed
======================
*/
void CL_AddReliableCommand( const char *cmd ) {
	int		index;

	// if we would be losing an old command that hasn't been acknowledged,
	// we must drop the connection
	if ( clc.reliableSequence - clc.reliableAcknowledge > MAX_RELIABLE_COMMANDS ) {
		Com_Error( ERR_DROP, "Client command overflow" );
	}
	clc.reliableSequence++;
	index = clc.reliableSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	if ( clc.reliableCommands[ index ] ) {
		Z_Free( clc.reliableCommands[ index ] );
	}
	clc.reliableCommands[ index ] = CopyString( cmd );
}

//======================================================================

/*
=================
CL_FlushMemory

Called by CL_MapLoading, CL_Connect_f, and CL_ParseGamestate the only
ways a client gets into a game
Also called by Com_Error
=================
*/
void CL_FlushMemory( void ) {

	// clear sounds (moved higher up within this func to avoid the odd sound stutter)
	S_DisableSounds();

	// unload the old VM
	CL_ShutdownCGame();

	CL_ShutdownUI();

	if ( re.Shutdown ) {
		re.Shutdown( qfalse, qfalse );		// don't destroy window or context
	}

	//rwwFIXMEFIXME: The game server appears to continue running, so clearing common bsp data causes crashing and other bad things
	/*
	CM_ClearMap();
	*/

	cls.soundRegistered = qfalse;
	cls.rendererStarted = qfalse;
}

/*
=====================
CL_MapLoading

A local server is starting to load a map, so update the
screen to let the user know about it, then dump all client
memory on the hunk from cgame, ui, and renderer
=====================
*/
void CL_MapLoading( void ) {
	if ( !com_cl_running->integer ) {
		return;
	}

	Con_Close();
	Key_SetCatcher( 0 );

	// if we are already connected to the local host, stay connected
	if ( cls.state >= CA_CONNECTED && !Q_stricmp( cls.servername, "localhost" ) )  {
		cls.state = CA_CONNECTED;		// so the connect screen is drawn
		memset( cls.updateInfoString, 0, sizeof( cls.updateInfoString ) );
//		memset( clc.serverMessage, 0, sizeof( clc.serverMessage ) );
		memset( &cl.gameState, 0, sizeof( cl.gameState ) );
		clc.lastPacketSentTime = -9999;
		SCR_UpdateScreen();
	} else {
		// clear nextmap so the cinematic shutdown doesn't execute it
		Cvar_Set( "nextmap", "" );
		CL_Disconnect();
		Q_strncpyz( cls.servername, "localhost", sizeof(cls.servername) );
		cls.state = CA_CHALLENGING;		// so the connect screen is drawn
		Key_SetCatcher( 0 );
		SCR_UpdateScreen();
		clc.connectTime = -RETRANSMIT_TIMEOUT;
		NET_StringToAdr( cls.servername, &clc.serverAddress);
		// we don't need a challenge on the localhost

		CL_CheckForResend();
	}

	CL_FlushMemory();
}

/*
=====================
CL_ClearState

Called before parsing a gamestate
=====================
*/
void CL_ClearState (void) {
	CL_ShutdownCGame();

	S_StopAllSounds();

	memset( &cl, 0, sizeof( cl ) );
}

/*
=====================
CL_FreeReliableCommands

Wipes all reliableCommands strings from clc
=====================
*/
void CL_FreeReliableCommands( void )
{
	// wipe the client connection
	for ( int i = 0 ; i < MAX_RELIABLE_COMMANDS ; i++ ) {
		if ( clc.reliableCommands[i] ) {
			Z_Free( clc.reliableCommands[i] );
		 	clc.reliableCommands[i] = NULL;
		}
	}
}


/*
=====================
CL_Disconnect

Called when a connection, or cinematic is being terminated.
Goes from a connected state to either a menu state or a console state
Sends a disconnect message to the server
This is also called on Com_Error and Com_Quit, so it shouldn't cause any errors
=====================
*/
void CL_Disconnect( void ) {
	if ( !com_cl_running || !com_cl_running->integer ) {
		return;
	}

	if (cls.uiStarted)
		UI_SetActiveMenu( NULL,NULL );

	SCR_StopCinematic ();
	S_ClearSoundBuffer();

	// send a disconnect message to the server
	// send it a few times in case one is dropped
	if ( cls.state >= CA_CONNECTED ) {
		CL_AddReliableCommand( "disconnect" );
		CL_WritePacket();
		CL_WritePacket();
		CL_WritePacket();
	}

	CL_ClearState ();

	CL_FreeReliableCommands();

	extern void CL_FreeServerCommands(void);
	CL_FreeServerCommands();

	memset( &clc, 0, sizeof( clc ) );

	cls.state = CA_DISCONNECTED;

	// allow cheats locally
	Cvar_Set( "timescale", "1" );//jic we were skipping
	Cvar_Set( "skippingCinematic", "0" );//jic we were skipping
}


/*
===================
CL_ForwardCommandToServer

adds the current command line as a clientCommand
things like godmode, noclip, etc, are commands directed to the server,
so when they are typed in at the console, they will need to be forwarded.
===================
*/
void CL_ForwardCommandToServer( void ) {
	const char	*cmd;
	char	string[MAX_STRING_CHARS];

	cmd = Cmd_Argv(0);

	// ignore key up commands
	if ( cmd[0] == '-' ) {
		return;
	}

	if ( cls.state != CA_ACTIVE || cmd[0] == '+' ) {
		Com_Printf ("Unknown command \"%s\"\n", cmd);
		return;
	}

	if ( Cmd_Argc() > 1 ) {
		Com_sprintf( string, sizeof(string), "%s %s", cmd, Cmd_Args() );
	} else {
		Q_strncpyz( string, cmd, sizeof(string) );
	}

	CL_AddReliableCommand( string );
}


/*
======================================================================

CONSOLE COMMANDS

======================================================================
*/

/*
==================
CL_ForwardToServer_f
==================
*/
void CL_ForwardToServer_f( void ) {
	if ( cls.state != CA_ACTIVE ) {
		Com_Printf ("Not connected to a server.\n");
		return;
	}

	// don't forward the first argument
	if ( Cmd_Argc() > 1 ) {
		CL_AddReliableCommand( Cmd_Args() );
	}
}

/*
==================
CL_Connect_f

connect <address[:port]>

The singleplayer client has only ever attached to its own server over the
in-memory loopback, via CL_MapLoading. This drives the same code path with a
real address. The server accepts unchallenged connects, so we enter
CA_CHALLENGING directly rather than requesting a challenge first -- exactly
as CL_MapLoading does for localhost.
==================
*/
extern void Menus_CloseAll( void );	// ui_shared.cpp
extern qboolean _UI_IsFullscreen( void );

// coop dev: press and release a key by name, as the keyboard would
static void CL_CoopKey_f( void ) {
	if ( Cmd_Argc() < 2 ) { Com_Printf( "usage: coop_key <keyname>\n" ); return; }
	const int key = Key_StringToKeynum( (char *)Cmd_Argv( 1 ) );
	if ( key < 0 ) { Com_Printf( "coop_key: unknown key %s\n", Cmd_Argv( 1 ) ); return; }
	CL_KeyEvent( key, qtrue, cls.realtime );
	CL_KeyEvent( key, qfalse, cls.realtime );
}

// coop dev: feed a mouse delta, as SDL would
static void CL_CoopMouse_f( void ) {
	if ( Cmd_Argc() < 3 ) { Com_Printf( "usage: coop_mouse <dx> <dy>\n" ); return; }
	CL_MouseEvent( atoi( Cmd_Argv( 1 ) ), atoi( Cmd_Argv( 2 ) ), cls.realtime );
}

// coop dev: the joiner's mouse went nowhere; show who owns input this instant
static void CL_CoopKeys_f( void ) {
	Com_Printf( "coop_keys: state %i catcher %i (console %i ui %i) uiFullscreen %i sensitivity %.2f cgameStarted %i cl_paused %i\n",
		cls.state, Key_GetCatcher(), ( Key_GetCatcher() & KEYCATCH_CONSOLE ) ? 1 : 0, ( Key_GetCatcher() & KEYCATCH_UI ) ? 1 : 0,
		_UI_IsFullscreen(), cl.cgameSensitivity, cls.cgameStarted, cl_paused->integer );
}

void CL_Connect_f( void ) {
	const char	*server;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: connect <address[:port]>\n" );
		return;
	}

	server = Cmd_Argv(1);

	// clear nextmap so the cinematic shutdown doesn't execute it
	Cvar_Set( "nextmap", "" );
	CL_Disconnect();
	Con_Close();

	Q_strncpyz( cls.servername, server, sizeof(cls.servername) );

	if ( !NET_StringToAdr( cls.servername, &clc.serverAddress ) ) {
		Com_Printf( "Bad server address: %s\n", cls.servername );
		cls.state = CA_DISCONNECTED;
		return;
	}

	if ( clc.serverAddress.port == 0 ) {
		clc.serverAddress.port = BigShort( (short)Cvar_VariableIntegerValue("net_port") );
	}

	Com_Printf( "%s resolved to %s\n", cls.servername, NET_AdrToString( clc.serverAddress ) );

	cls.state = CA_CHALLENGING;		// so the connect screen is drawn
	Key_SetCatcher( 0 );
	// coop: CL_Disconnect's UI_SetActiveMenu(NULL) only drops the key catcher (and is a no-op while the menu's
	// intro video plays); a main menu left visible makes SCR_DrawScreenField skip the connect screen
	// while nothing paints it: the swap buffers alternated logo / menu until the host answered
	Menus_CloseAll();
	SCR_UpdateScreen();
	clc.connectTime = -RETRANSMIT_TIMEOUT;	// send the connect packet immediately

	CL_CheckForResend();
}

/*
==================
CL_Disconnect_f
==================
*/
void CL_Disconnect_f( void ) {
	SCR_StopCinematic();

	//FIXME:
	// TA codebase added additional CA_CINEMATIC check below, presumably so they could play cinematics
	//	in the menus when disconnected, although having the SCR_StopCinematic() call above is weird.
	// Either there's a bug, or the new version of that function conditionally-doesn't stop cinematics...
	//
	if ( cls.state != CA_DISCONNECTED && cls.state != CA_CINEMATIC ) {
		Com_Error (ERR_DISCONNECT, "Disconnected from server");
	}
}


/*
=================
CL_Vid_Restart_f

Restart the video subsystem
=================
*/
void CL_Vid_Restart_f( void ) {
	S_StopAllSounds();		// don't let them loop during the restart
	S_BeginRegistration();	// all sound handles are now invalid
	CL_ShutdownRef(qtrue);
	CL_ShutdownUI();
	CL_ShutdownCGame();

	//rww - sof2mp does this here, but it seems to cause problems in this codebase.
//	CM_ClearMap();

	cls.rendererStarted = qfalse;
	cls.uiStarted = qfalse;
	cls.cgameStarted = qfalse;
	cls.soundRegistered = qfalse;

	CL_InitRef();

	CL_StartHunkUsers();

	// unpause so the cgame definately gets a snapshot and renders a frame
	Cvar_Set( "cl_paused", "0" );
}

/*
=================
CL_Snd_Restart_f

Restart the sound subsystem
The cgame and game must also be forced to restart because
handles will be invalid
=================
*/
void CL_Snd_Restart_f( void ) {
	S_Shutdown();

	S_Init();

//	CL_Vid_Restart_f();

	extern qboolean	s_soundMuted;
	s_soundMuted = qfalse;		// we can play again

	S_RestartMusic();

	extern void S_ReloadAllUsedSounds(void);
	S_ReloadAllUsedSounds();

	extern void AS_ParseSets(void);
	AS_ParseSets();
}
/*
==================
CL_Configstrings_f
==================
*/
void CL_Configstrings_f( void ) {
	int		i;
	int		ofs;

	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "Not connected to a server.\n");
		return;
	}

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		ofs = cl.gameState.stringOffsets[ i ];
		if ( !ofs ) {
			continue;
		}
		Com_Printf( "%4i: %s\n", i, cl.gameState.stringData + ofs );
	}
}

/*
==============
CL_Clientinfo_f
==============
*/
void CL_Clientinfo_f( void ) {
	Com_Printf( "--------- Client Information ---------\n" );
	Com_Printf( "state: %i\n", cls.state );
	Com_Printf( "Server: %s\n", cls.servername );
	Com_Printf ("User info settings:\n");
	Info_Print( Cvar_InfoString( CVAR_USERINFO ) );
	Com_Printf( "--------------------------------------\n" );
}


//====================================================================

void UI_UpdateConnectionString( const char *string );

/*
=================
CL_CheckForResend

Resend a connect message if the last one has timed out
=================
*/
void CL_CheckForResend( void ) {
	int		port;
	char	info[MAX_INFO_STRING];

//	if ( cls.state == CA_CINEMATIC )
	if ( cls.state == CA_CINEMATIC || CL_IsRunningInGameCinematic())
	{
		return;
	}

	// resend if we haven't gotten a reply yet
	if ( cls.state < CA_CONNECTING || cls.state > CA_CHALLENGING ) {
		return;
	}

	if ( cls.realtime - clc.connectTime < RETRANSMIT_TIMEOUT ) {
		return;
	}

	clc.connectTime = cls.realtime;	// for retransmit requests
	clc.connectPacketCount++;

	// requesting a challenge
	switch ( cls.state ) {
	case CA_CONNECTING:
		UI_UpdateConnectionString( va("(%i)", clc.connectPacketCount ) );

		NET_OutOfBandPrint(NS_CLIENT, clc.serverAddress, "getchallenge");
		break;

	case CA_CHALLENGING:
	// sending back the challenge
		port = Cvar_VariableIntegerValue("net_qport");

		UI_UpdateConnectionString( va("(%i)", clc.connectPacketCount ) );

		Q_strncpyz( info, Cvar_InfoString( CVAR_USERINFO ), sizeof( info ) );
		Info_SetValueForKey( info, "protocol", va("%i", PROTOCOL_VERSION ) );
		Info_SetValueForKey( info, "qport", va("%i", port ) );
		Info_SetValueForKey( info, "challenge", va("%i", clc.challenge ) );
		NET_OutOfBandPrint( NS_CLIENT, clc.serverAddress, "connect \"%s\"", info );
		// the most current userinfo has been sent, so watch for any
		// newer changes to userinfo variables
		cvar_modifiedFlags &= ~CVAR_USERINFO;
		break;

	default:
		Com_Error( ERR_FATAL, "CL_CheckForResend: bad cls.state" );
	}
}


/*
===================
CL_DisconnectPacket

Sometimes the server can drop the client and the netchan based
disconnect can be lost.  If the client continues to send packets
to the server, the server will send out of band disconnect packets
to the client so it doesn't have to wait for the full timeout period.
===================
*/
void CL_DisconnectPacket( netadr_t from ) {
	if ( cls.state != CA_ACTIVE ) {
		return;
	}

	// if not from our server, ignore it
	if ( !NET_CompareAdr( from, clc.netchan.remoteAddress ) ) {
		return;
	}

	// if we have received packets within three seconds, ignore it
	// (it might be a malicious spoof)
	if ( cls.realtime - clc.lastPacketTime < 3000 ) {
		return;
	}

	// drop the connection (FIXME: connection dropped dialog)
	Com_Printf( "Server disconnected for unknown reason\n" );
	CL_Disconnect();
}


/*
=================
CL_ConnectionlessPacket

Responses to broadcasts, etc
=================
*/
/*
===================
CL_ServerInfoPacket

D2: parse an infoResponse from a co-op host (a reply to our `localservers`
broadcast) and record it in cls.localServers, deduped by address.
===================
*/
void CL_ServerInfoPacket( netadr_t from, msg_t *msg ) {
	const char	*infoString = MSG_ReadString( msg );

	// Reject spoofed / stale replies: must echo our challenge, speak our
	// protocol, and identify as a jk2 co-op host (filters out stock JA servers).
	if ( atoi( Info_ValueForKey( infoString, "challenge" ) ) != cls.localServerChallenge ) {
		return;
	}
	if ( atoi( Info_ValueForKey( infoString, "protocol" ) ) != PROTOCOL_VERSION ) {
		return;
	}
	if ( Q_stricmp( Info_ValueForKey( infoString, "game" ), "jacoop" ) != 0 ) {
		return;
	}

	// Dedupe by address — the broadcast is sent more than once.
	int i;
	for ( i = 0; i < cls.numLocalServers; i++ ) {
		if ( NET_CompareAdr( cls.localServers[i].adr, from ) ) {
			return;
		}
	}
	if ( cls.numLocalServers >= MAX_LOCAL_SERVERS ) {
		return;
	}

	localServer_t *ls = &cls.localServers[cls.numLocalServers++];
	memset( ls, 0, sizeof( *ls ) );
	ls->adr = from;
	Q_strncpyz( ls->hostname, Info_ValueForKey( infoString, "hostname" ), sizeof( ls->hostname ) );
	Q_strncpyz( ls->mapname,  Info_ValueForKey( infoString, "mapname" ),  sizeof( ls->mapname ) );
	ls->clients    = atoi( Info_ValueForKey( infoString, "clients" ) );
	ls->maxClients = atoi( Info_ValueForKey( infoString, "sv_maxclients" ) );

	// This print is D2's test surface; D3's menu renders the same array.
	Com_Printf( "co-op host: %s  (%s, %i/%i players)  %s\n",
		ls->hostname[0] ? ls->hostname : "?",
		ls->mapname[0] ? ls->mapname : "?",
		ls->clients, ls->maxClients,
		NET_AdrToString( from ) );
}

/*
===================
CL_LocalServers_f

D2: `localservers` — broadcast a getinfo probe to the co-op port range on the
LAN so hosts advertise themselves (they reply with infoResponse, handled in
CL_ServerInfoPacket). Clears the previous results first.
===================
*/
void CL_LocalServers_f( void ) {
	Com_Printf( "Scanning for co-op hosts on the LAN...\n" );

	cls.numLocalServers = 0;
	cls.localServerChallenge = rand() ^ ( rand() << 16 ) ^ Sys_Milliseconds();

	// Networking must be up to send the broadcast.
	if ( !NET_IsSocketOpen() ) {
		Cvar_Set( "net_enabled", "1" );
		NET_Restart();
	}
	if ( !NET_IsSocketOpen() ) {
		Com_Printf( "localservers: could not open a network socket.\n" );
		return;
	}

	netadr_t to;
	memset( &to, 0, sizeof( to ) );
	to.type = NA_BROADCAST;

	// Probe the co-op port scan range (matches NET_OpenIP / coop_host: 29070-29079).
	const char *msg = va( "getinfo %i", cls.localServerChallenge );
	for ( int port = 29070; port <= 29079; port++ ) {
		to.port = BigShort( (short)port );
		NET_OutOfBandPrint( NS_CLIENT, to, "%s", msg );
	}
}

/*
===================
CL co-op server-list accessors (D3)

The in-engine SP UI reads the LAN-discovered hosts (cls.localServers, filled by
D2's localservers command) through these. Kept here because cls lives in this
module; the UI extern-declares them.
===================
*/
int CL_GetCoopServerCount( void ) {
	return cls.numLocalServers;
}

// Fill 'out' with a display line for row 'index' ("Host - map (n/m)"). Returns
// the empty string for an out-of-range row.
const char *CL_GetCoopServerText( int index ) {
	static char line[MAX_INFO_STRING];
	if ( index < 0 || index >= cls.numLocalServers ) {
		return "";
	}
	const localServer_t *ls = &cls.localServers[index];
	Com_sprintf( line, sizeof( line ), "%s  -  %s  (%i/%i)",
		ls->hostname[0] ? ls->hostname : "?",
		ls->mapname[0] ? ls->mapname : "?",
		ls->clients, ls->maxClients );
	return line;
}

// coop lobby feeder: the host publishes "L|name\tready\tmodel|..." in
// CS_COOP_LOBBY (L = lobby, G = playing); one row per connected player.
static int CL_CoopLobbyRows( char rows[MAX_CLIENTS][96] ) {
	int n = 0;
	if ( cls.state < CA_LOADING || !cl.gameState.stringOffsets[CS_COOP_LOBBY] ) {
		return 0;
	}
	const char *s = cl.gameState.stringData + cl.gameState.stringOffsets[CS_COOP_LOBBY];
	const char *p = strchr( s, '|' );
	while ( p && n < MAX_CLIENTS ) {
		p++;
		const char *end = strchr( p, '|' );
		char row[96];
		Q_strncpyz( row, p, end ? Q_min( (int)( end - p ) + 1, (int)sizeof( row ) ) : (int)sizeof( row ) );
		char *tab = strchr( row, '\t' );
		int ready = 0;
		if ( tab ) {
			*tab = '\0';
			ready = atoi( tab + 1 );
		}
		Com_sprintf( rows[n++], 96, "%s%s", row, ready == 2 ? "   (hote)" : ready == 1 ? "   -  pret" : "   -  pas pret" );
		p = end;
	}
	return n;
}

int CL_GetCoopLobbyCount( void ) {
	char rows[MAX_CLIENTS][96];
	return CL_CoopLobbyRows( rows );
}

const char *CL_GetCoopLobbyText( int index ) {
	static char rows[MAX_CLIENTS][96];
	const int n = CL_CoopLobbyRows( rows );
	return ( index >= 0 && index < n ) ? rows[index] : "";
}

// Fill 'out' with "ip:port" for row 'index'. Returns qfalse if out of range.
qboolean CL_GetCoopServerAddress( int index, char *out, int outSize ) {
	if ( index < 0 || index >= cls.numLocalServers || !out || outSize <= 0 ) {
		return qfalse;
	}
	Q_strncpyz( out, NET_AdrToString( cls.localServers[index].adr ), outSize );
	return qtrue;
}

void CL_ConnectionlessPacket( netadr_t from, msg_t *msg ) {
	char	*s;
	const char	*c;

	MSG_BeginReading( msg );
	MSG_ReadLong( msg );	// skip the -1

	s = MSG_ReadStringLine( msg );

	Cmd_TokenizeString( s );

	c = Cmd_Argv(0);

	Com_DPrintf ("CL packet %s: %s\n", NET_AdrToString(from), c);

	// challenge from the server we are connecting to
	if ( !strcmp(c, "challengeResponse") ) {
		if ( cls.state != CA_CONNECTING ) {
			Com_Printf( "Unwanted challenge response received.  Ignored.\n" );
		} else {
			// start sending challenge repsonse instead of challenge request packets
			clc.challenge = atoi(Cmd_Argv(1));
			cls.state = CA_CHALLENGING;
			clc.connectPacketCount = 0;
			clc.connectTime = -99999;

			// take this address as the new server address.  This allows
			// a server proxy to hand off connections to multiple servers
			clc.serverAddress = from;
		}
		return;
	}

	// server connection
	if ( !strcmp(c, "connectResponse") ) {
		if ( cls.state >= CA_CONNECTED ) {
			Com_Printf ("Dup connect received.  Ignored.\n");
			return;
		}
		if ( cls.state != CA_CHALLENGING ) {
			Com_Printf ("connectResponse packet while not connecting.  Ignored.\n");
			return;
		}
		if ( !NET_CompareBaseAdr( from, clc.serverAddress ) ) {
			Com_Printf( "connectResponse from a different address.  Ignored.\n" );
			Com_Printf( "%s should have been %s\n", NET_AdrToString( from ),
				NET_AdrToString( clc.serverAddress ) );
			return;
		}
		Netchan_Setup (NS_CLIENT, &clc.netchan, from, Cvar_VariableIntegerValue( "net_qport" ) );
		cls.state = CA_CONNECTED;
		clc.lastPacketSentTime = -9999;		// send first packet immediately
		return;
	}

	// a disconnect message from the server, which will happen if the server
	// dropped the connection but it is still getting packets from us
	if (!strcmp(c, "disconnect")) {
		CL_DisconnectPacket( from );
		return;
	}

	// echo request from server
	if ( !strcmp(c, "echo") ) {
		NET_OutOfBandPrint( NS_CLIENT, from, "%s", Cmd_Argv(1) );
		return;
	}

	// print request from server
	if ( !strcmp(c, "print") ) {
		s = MSG_ReadString( msg );
		UI_UpdateConnectionMessageString( s );
		Com_Printf( "%s", s );
		// E3: if we are still trying to connect and this print came from the
		// server we are dialling, it is a rejection ("Server is full.",
		// "Server uses protocol version N.", etc). Stock behaviour only printed
		// it to the console and kept resending the connect request, so the
		// player just sat on the loading screen forever with no on-screen
		// reason. Treat it as a connection failure: stop retrying and surface
		// the server's message on the menu (ERR_DISCONNECT drops to the main
		// menu showing com_errorMessage).
		if ( ( cls.state == CA_CONNECTING || cls.state == CA_CHALLENGING )
			&& NET_CompareAdr( from, clc.serverAddress ) ) {
			Com_Error( ERR_DISCONNECT, "%s", s );
		}
		return;
	}

	// D2: reply to our `localservers` broadcast — a co-op host advertising
	// itself. Verify it is one of ours (challenge + protocol + game=jacoop),
	// then record it (deduped by address) for the co-op browser.
	if ( !strcmp(c, "infoResponse") ) {
		CL_ServerInfoPacket( from, msg );
		return;
	}


	Com_DPrintf ("Unknown connectionless packet command.\n");
}


/*
=================
CL_PacketEvent

A packet has arrived from the main event loop
=================
*/
void CL_PacketEvent( netadr_t from, msg_t *msg ) {
	clc.lastPacketTime = cls.realtime;

	if ( msg->cursize >= 4 && *(int *)msg->data == -1 ) {
		CL_ConnectionlessPacket( from, msg );
		return;
	}

	if ( cls.state < CA_CONNECTED ) {
		return;		// can't be a valid sequenced packet
	}

	if ( msg->cursize < 8 ) {
		Com_Printf ("%s: Runt packet\n",NET_AdrToString( from ));
		return;
	}

	//
	// packet from server
	//
	if ( !NET_CompareAdr( from, clc.netchan.remoteAddress ) ) {
		Com_DPrintf ("%s:sequenced packet without connection\n"
			,NET_AdrToString( from ) );
		// FIXME: send a client disconnect?
		return;
	}

	if (!Netchan_Process( &clc.netchan, msg) ) {
		return;		// out of order, duplicated, etc
	}

	clc.lastPacketTime = cls.realtime;
	CL_ParseServerMessage( msg );
}

/*
==================
CL_CheckTimeout

==================
*/
void CL_CheckTimeout( void ) {
	//
	// check timeout
	//
	if ( ( !CL_CheckPaused() || !sv_paused->integer )
//		&& cls.state >= CA_CONNECTED && cls.state != CA_CINEMATIC
		&& cls.state >= CA_CONNECTED && (cls.state != CA_CINEMATIC && !CL_IsRunningInGameCinematic())
		&& cls.realtime - clc.lastPacketTime > cl_timeout->value*1000) {
		if (++cl.timeoutcount > 5) {	// timeoutcount saves debugger
			Com_Printf ("\nServer connection timed out.\n");
			CL_Disconnect ();
			return;
		}
	} else {
		cl.timeoutcount = 0;
	}
}

/*
==================
CL_CheckPaused
Check whether client has been paused.
==================
*/
qboolean CL_CheckPaused(void)
{
	// if cl_paused->modified is set, the cvar has only been changed in
	// this frame. Keep paused in this frame to ensure the server doesn't
	// lag behind.
	if ( !com_sv_running->integer )
		return qfalse;	// coop: a remote client never pauses (the host runs the game)
	if(cl_paused->integer || cl_paused->modified)
		return qtrue;

	return qfalse;
}

//============================================================================

/*
==================
CL_CheckUserinfo

==================
*/
void CL_CheckUserinfo( void ) {
	if ( cls.state < CA_CHALLENGING ) {
		return;
	}
	// don't overflow the reliable command buffer when paused
	if ( CL_CheckPaused() ) {
		return;
	}
	// send a reliable userinfo update if needed
	if ( cvar_modifiedFlags & CVAR_USERINFO ) {
		cvar_modifiedFlags &= ~CVAR_USERINFO;
		CL_AddReliableCommand( va("userinfo \"%s\"", Cvar_InfoString( CVAR_USERINFO ) ) );
	}

}


/*
==================
CL_Frame

==================
*/
extern cvar_t	*cl_newClock;
static unsigned int frameCount;
float avgFrametime=0.0;
void CL_Frame ( int msec,float fractionMsec ) {

	if ( !com_cl_running->integer ) {
		return;
	}

	// load the ref / cgame if needed
	CL_StartHunkUsers();

	if ( cls.state == CA_DISCONNECTED && !( Key_GetCatcher( ) & KEYCATCH_UI )
		&& !com_sv_running->integer ) {
		// if disconnected, bring up the menu
		if (!CL_CheckPendingCinematic())	// this avoid having the menu flash for one frame before pending cinematics
		{
			UI_SetActiveMenu( "mainMenu",NULL );
		}
	}


	// if recording an avi, lock to a fixed fps
	if ( cl_avidemo->integer ) {
		// save the current screen
		if ( cls.state == CA_ACTIVE ) {
			if (cl_avidemo->integer > 0) {
				Cbuf_ExecuteText( EXEC_NOW, "screenshot silent\n" );
			} else {
				Cbuf_ExecuteText( EXEC_NOW, "screenshot_tga silent\n" );
			}
		}
		// fixed time for next frame
		if (cl_avidemo->integer > 0) {
			msec = 1000 / cl_avidemo->integer;
		} else {
			msec = 1000 / -cl_avidemo->integer;
		}
	}

	// save the msec before checking pause
	cls.realFrametime = msec;

	// decide the simulation time
	cls.frametime = msec;
	if(cl_framerate->integer)
	{
		avgFrametime+=msec;
		char mess[256];
		if(!(frameCount&0x1f))
		{
			sprintf(mess,"Frame rate=%f\n\n",1000.0f*(1.0/(avgFrametime/32.0f)));
	//		OutputDebugString(mess);
			Com_Printf(mess);
			avgFrametime=0.0f;
		}
		frameCount++;
	}
	cls.frametimeFraction=fractionMsec;
	cls.realtime += msec;
	cls.realtimeFraction+=fractionMsec;
	if (cls.realtimeFraction>=1.0f)
	{
		if (cl_newClock&&cl_newClock->integer)
		{
			cls.realtime++;
		}
		cls.realtimeFraction-=1.0f;
	}
	if ( cl_timegraph->integer ) {
		SCR_DebugGraph ( cls.realFrametime * 0.25, 0 );
	}

	// see if we need to update any userinfo
	CL_CheckUserinfo();

	// if we haven't gotten a packet in a long time,
	// drop the connection
	CL_CheckTimeout();

	// send intentions now
	CL_SendCmd();

	// resend a connection request if necessary
	CL_CheckForResend();

	// decide on the serverTime to render
	CL_SetCGameTime();

	if (cl_pano->integer && cls.state == CA_ACTIVE) {	//grab some panoramic shots
		int i = 1;
		int pref = cl_pano->integer;
		int oldnoprint = cl_noprint->integer;
		Con_Close();
		cl_noprint->integer = 1;	//hide the screen shot msgs
		for (; i <= cl_panoNumShots->integer; i++) {
			Cvar_SetValue( "pano", i );
			SCR_UpdateScreen();// update the screen
			Cbuf_ExecuteText( EXEC_NOW, va("screenshot %dpano%02d\n", pref, i) );	//grab this screen
		}
		Cvar_SetValue( "pano", 0 );	//done
		cl_noprint->integer = oldnoprint;
	}

	if (cl_skippingcin->integer && !cl_endcredits->integer && !com_developer->integer ) {
		if (cl_skippingcin->modified){
			S_StopSounds();		//kill em all but music
			cl_skippingcin->modified=qfalse;
			Com_Printf (S_COLOR_YELLOW "%s", SE_GetString("CON_TEXT_SKIPPING"));
			SCR_UpdateScreen();
		}
	} else {
		// update the screen
		SCR_UpdateScreen();
	}
	// update audio
	S_Update();

	// advance local effects for next frame
	SCR_RunCinematic();

	Con_RunConsole();

	cls.framecount++;
}

//============================================================================

/*
============
CL_ShutdownRef
============
*/
static void CL_ShutdownRef( qboolean restarting ) {
	if ( re.Shutdown ) {
		re.Shutdown( qtrue, restarting );
	}

	memset( &re, 0, sizeof( re ) );

	if ( rendererLib != NULL ) {
		Sys_UnloadDll (rendererLib);
		rendererLib = NULL;
	}
}

/*
============================
CL_StartSound

Convenience function for the sound system to be started
REALLY early on Xbox, helps with memory fragmentation.
============================
*/
void CL_StartSound( void ) {
	if ( !cls.soundStarted ) {
		cls.soundStarted = qtrue;
		S_Init();
	}

	if ( !cls.soundRegistered ) {
		cls.soundRegistered = qtrue;
		S_BeginRegistration();
	}
}

/*
============
CL_InitRenderer
============
*/
void CL_InitRenderer( void ) {
	// this sets up the renderer and calls R_Init
	re.BeginRegistration( &cls.glconfig );

	// coop: on a serverless remote client, register the humanoid skeleton and
	// the map's cinematic skeleton back to back, before the UI or the cgame can
	// load either. Ghoul2 finds the cinematic GLA as "handle of _humanoid.gla + 1"
	// (G2_API.cpp animModelIndexOffset), and the anim-set parser refuses to
	// continue otherwise. The host gets this ordering for free from
	// SV_SpawnServer -> first humanoid spawn; here the renderer was just reset by
	// CL_FlushMemory on the gamestate, so these become the first two models.
	if ( !com_sv_running->integer && cl.gameState.stringOffsets[CS_SERVERINFO] ) {
		const char *serverinfo = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
		const char *mapname = Info_ValueForKey( serverinfo, "mapname" );
		if ( mapname[0] ) {
			const char *slash = strrchr( mapname, '/' );
			if ( slash ) {
				mapname = slash + 1;
			}
			re.RegisterModel( "models/players/_humanoid/_humanoid.gla" );
			re.RegisterModel( va( "models/players/_humanoid_%s/_humanoid_%s.gla", mapname, mapname ) );
		}
	}

	// load character sets
	cls.charSetShader = re.RegisterShaderNoMip("gfx/2d/charsgrid_med");
	cls.consoleFont = re.RegisterFont( "ocr_a" );
	cls.whiteShader = re.RegisterShader( "white" );
	cls.consoleShader = re.RegisterShader( "console" );
	g_console_field_width = cls.glconfig.vidWidth / SMALLCHAR_WIDTH - 2;
	g_consoleField.widthInChars = g_console_field_width;
}

/*
============================
CL_StartHunkUsers

After the server has cleared the hunk, these will need to be restarted
This is the only place that any of these functions are called from
============================
*/
void CL_StartHunkUsers( void ) {
	if ( !com_cl_running->integer ) {
		return;
	}

	if ( !cls.rendererStarted ) {
		cls.rendererStarted = qtrue;
		CL_InitRenderer();
	}

	if ( !cls.soundStarted ) {
		cls.soundStarted = qtrue;
		S_Init();
	}

	if ( !cls.soundRegistered ) {
		cls.soundRegistered = qtrue;
		S_BeginRegistration();
	}

	//we require the ui to be loaded here or else it crashes trying to access the ui on command line map loads
	if ( !cls.uiStarted ) {
		cls.uiStarted = qtrue;
		CL_InitUI();
	}

//	if ( !cls.cgameStarted && cls.state > CA_CONNECTED && cls.state != CA_CINEMATIC ) {
	if ( !cls.cgameStarted && cls.state > CA_CONNECTED && (cls.state != CA_CINEMATIC && !CL_IsRunningInGameCinematic()) )
	{
		cls.cgameStarted = qtrue;
		CL_InitCGame();
	}
}

/*
================
CL_RefPrintf

DLL glue
================
*/
void QDECL CL_RefPrintf( int print_level, const char *fmt, ...) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	if ( print_level == PRINT_ALL ) {
		Com_Printf ("%s", msg);
	} else if ( print_level == PRINT_WARNING ) {
		Com_Printf (S_COLOR_YELLOW "%s", msg);		// yellow
	} else if ( print_level == PRINT_DEVELOPER ) {
		Com_DPrintf (S_COLOR_RED "%s", msg);		// red
	}
}

/*
============
String_GetStringValue

DLL glue, but highly reusuable DLL glue at that
============
*/

const char *String_GetStringValue( const char *reference )
{
#ifndef JK2_MODE
	return SE_GetString(reference);
#else
	return JK2SP_GetStringTextString(reference);
#endif
}

extern qboolean gbAlreadyDoingLoad;
extern void *gpvCachedMapDiskImage;
extern char  gsCachedMapDiskImage[MAX_QPATH];
extern qboolean gbUsingCachedMapDataRightNow;

char *get_gsCachedMapDiskImage( void )
{
	return gsCachedMapDiskImage;
}

void *get_gpvCachedMapDiskImage( void )
{
	return gpvCachedMapDiskImage;
}

qboolean *get_gbUsingCachedMapDataRightNow( void )
{
	return &gbUsingCachedMapDataRightNow;
}

qboolean *get_gbAlreadyDoingLoad( void )
{
	return &gbAlreadyDoingLoad;
}

int get_com_frameTime( void )
{
	return com_frameTime;
}

void *CL_Malloc(int iSize, memtag_t eTag, qboolean bZeroit, int iAlign)
{
    return Z_Malloc(iSize, eTag, bZeroit);
}

/*
============
CL_InitRef
============
*/
extern qboolean S_FileExists( const char *psFilename );
extern bool CM_CullWorldBox (const cplane_t *frustum, const vec3pair_t bounds);
extern qboolean SND_RegisterAudio_LevelLoadEnd(qboolean bDeleteEverythingNotUsedThisLevel /* 99% qfalse */);
extern cvar_t *Cvar_Set2( const char *var_name, const char *value, qboolean force);
extern CMiniHeap *G2VertSpaceServer;
static CMiniHeap *GetG2VertSpaceServer( void ) {
	return G2VertSpaceServer;
}

// NOTENOTE: If you change the output name of rd-vanilla, change this define too!
#ifdef JK2_MODE
#define DEFAULT_RENDER_LIBRARY	"rdjosp-vanilla"
#else
#define DEFAULT_RENDER_LIBRARY	"rdsp-vanilla"
#endif

void CL_InitRef( void ) {
	refexport_t	*ret;
	static refimport_t rit;
	char		dllName[MAX_OSPATH];
	GetRefAPI_t	GetRefAPI;

	Com_Printf( "----- Initializing Renderer ----\n" );
    cl_renderer = Cvar_Get( "cl_renderer", DEFAULT_RENDER_LIBRARY, CVAR_ARCHIVE|CVAR_LATCH );

	Com_sprintf( dllName, sizeof( dllName ), "%s_" ARCH_STRING DLL_EXT, cl_renderer->string );

	if( !(rendererLib = Sys_LoadDll( dllName, qfalse )) && strcmp( cl_renderer->string, cl_renderer->resetString ) )
	{
		Com_Printf( "failed: trying to load fallback renderer\n" );
		Cvar_ForceReset( "cl_renderer" );

		Com_sprintf( dllName, sizeof( dllName ), DEFAULT_RENDER_LIBRARY "_" ARCH_STRING DLL_EXT );
		rendererLib = Sys_LoadDll( dllName, qfalse );
	}

	if ( !rendererLib ) {
		Com_Error( ERR_FATAL, "Failed to load renderer\n" );
	}

	memset( &rit, 0, sizeof( rit ) );

	GetRefAPI = (GetRefAPI_t)Sys_LoadFunction( rendererLib, "GetRefAPI" );
	if ( !GetRefAPI )
		Com_Error( ERR_FATAL, "Can't load symbol GetRefAPI: '%s'", Sys_LibraryError() );

#define RIT(y)	rit.y = y
	RIT(CIN_PlayCinematic);
	RIT(CIN_RunCinematic);
	RIT(CIN_UploadCinematic);
	RIT(CL_IsRunningInGameCinematic);
	RIT(Cmd_AddCommand);
	RIT(Cmd_Argc);
	RIT(Cmd_ArgsBuffer);
	RIT(Cmd_Argv);
	RIT(Cmd_ExecuteString);
	RIT(Cmd_RemoveCommand);
	RIT(CM_ClusterPVS);
	RIT(CM_CullWorldBox);
	RIT(CM_DeleteCachedMap);
	RIT(CM_DrawDebugSurface);
	RIT(CM_PointContents);
	RIT(Cvar_Get);
	RIT(Cvar_Set);
	RIT(Cvar_SetValue);
	RIT(Cvar_CheckRange);
	RIT(Cvar_VariableIntegerValue);
	RIT(Cvar_VariableString);
	RIT(Cvar_VariableStringBuffer);
	RIT(Cvar_VariableValue);
	RIT(FS_FCloseFile);
	RIT(FS_FileIsInPAK);
	RIT(FS_FOpenFileByMode);
	RIT(FS_FOpenFileRead);
	RIT(FS_FOpenFileWrite);
	RIT(FS_FreeFile);
	RIT(FS_FreeFileList);
	RIT(FS_ListFiles);
	RIT(FS_Read);
	RIT(FS_ReadFile);
	RIT(FS_Write);
	RIT(FS_WriteFile);
	RIT(Hunk_ClearToMark);
	RIT(SND_RegisterAudio_LevelLoadEnd);
	//RIT(SV_PointContents);
	RIT(SV_Trace);
	RIT(S_RestartMusic);
	RIT(Z_Free);
	rit.Malloc=CL_Malloc;
	RIT(Z_MemSize);
	RIT(Z_MorphMallocTag);

	RIT(Hunk_ClearToMark);

    rit.WIN_Init = WIN_Init;
	rit.WIN_SetGamma = WIN_SetGamma;
    rit.WIN_Shutdown = WIN_Shutdown;
    rit.WIN_Present = WIN_Present;
	rit.GL_GetProcAddress = WIN_GL_GetProcAddress;
	rit.GL_ExtensionSupported = WIN_GL_ExtensionSupported;

	rit.PD_Load = PD_Load;
	rit.PD_Store = PD_Store;

	rit.Error = Com_Error;
	rit.FS_FileExists = S_FileExists;
	rit.GetG2VertSpaceServer = GetG2VertSpaceServer;
	rit.LowPhysicalMemory = Sys_LowPhysicalMemory;
	rit.Milliseconds = Sys_Milliseconds2;
	rit.Printf = CL_RefPrintf;
	rit.SE_GetString = String_GetStringValue;

	rit.SV_Trace = SV_Trace;

	rit.gpvCachedMapDiskImage = get_gpvCachedMapDiskImage;
	rit.gsCachedMapDiskImage = get_gsCachedMapDiskImage;
	rit.gbUsingCachedMapDataRightNow = get_gbUsingCachedMapDataRightNow;
	rit.gbAlreadyDoingLoad = get_gbAlreadyDoingLoad;
	rit.com_frameTime = get_com_frameTime;

	rit.SV_PointContents = SV_PointContents;

	rit.saved_game = &ojk::SavedGame::get_instance();

	ret = GetRefAPI( REF_API_VERSION, &rit );

	if ( !ret ) {
		Com_Error (ERR_FATAL, "Couldn't initialize refresh" );
	}

	re = *ret;

	Com_Printf( "-------------------------------\n");

	// unpause so the cgame definately gets a snapshot and renders a frame
	Cvar_Set( "cl_paused", "0" );
}


//===========================================================================================

void CL_CompleteCinematic( char *args, int argNum );

/*
====================
CL_Init
====================
*/
void CL_Init( void ) {
	Com_Printf( "----- Client Initialization -----\n" );

#ifdef JK2_MODE
	JK2SP_Register("con_text", SP_REGISTER_REQUIRED);	//reference is CON_TEXT
	JK2SP_Register("keynames", SP_REGISTER_REQUIRED);	// reference is KEYNAMES
#endif

	Con_Init ();

	CL_ClearState ();

	cls.state = CA_DISCONNECTED;	// no longer CA_UNINITIALIZED
	//cls.keyCatchers = KEYCATCH_CONSOLE;
	cls.realtime = 0;
	cls.realtimeFraction=0.0f;	// fraction of a msec accumulated

	CL_InitInput ();

	//
	// register our variables
	//
	cl_noprint = Cvar_Get( "cl_noprint", "0", 0 );

	cl_timeout = Cvar_Get ("cl_timeout", "125", 0);

	cl_timeNudge = Cvar_Get ("cl_timeNudge", "0", CVAR_TEMP );
	cl_shownet = Cvar_Get ("cl_shownet", "0", CVAR_TEMP );
	cl_showTimeDelta = Cvar_Get ("cl_showTimeDelta", "0", CVAR_TEMP );
	cl_newClock = Cvar_Get ("cl_newClock", "1", 0);
	cl_activeAction = Cvar_Get( "activeAction", "", CVAR_TEMP );

	cl_avidemo = Cvar_Get ("cl_avidemo", "0", 0);
	cl_pano = Cvar_Get ("pano", "0", 0);
	cl_panoNumShots= Cvar_Get ("panoNumShots", "10", CVAR_ARCHIVE_ND);
	cl_skippingcin = Cvar_Get ("skippingCinematic", "0", CVAR_ROM);
	cl_endcredits = Cvar_Get ("cg_endcredits", "0", 0);

	cl_yawspeed = Cvar_Get ("cl_yawspeed", "140", CVAR_ARCHIVE_ND);
	cl_pitchspeed = Cvar_Get ("cl_pitchspeed", "140", CVAR_ARCHIVE_ND);
	cl_anglespeedkey = Cvar_Get ("cl_anglespeedkey", "1.5", CVAR_ARCHIVE_ND);

	cl_packetdup = Cvar_Get ("cl_packetdup", "1", CVAR_ARCHIVE_ND );

	cl_run = Cvar_Get ("cl_run", "1", CVAR_ARCHIVE_ND);
	cl_sensitivity = Cvar_Get ("sensitivity", "5", CVAR_ARCHIVE);
	cl_mouseAccel = Cvar_Get ("cl_mouseAccel", "0", CVAR_ARCHIVE_ND);
	cl_freelook = Cvar_Get( "cl_freelook", "1", CVAR_ARCHIVE_ND );

	cl_showMouseRate = Cvar_Get ("cl_showmouserate", "0", 0);

	cl_allowAltEnter = Cvar_Get ("cl_allowAltEnter", "1", CVAR_ARCHIVE_ND);
	cl_inGameVideo = Cvar_Get ("cl_inGameVideo", "1", CVAR_ARCHIVE_ND);
	cl_framerate	= Cvar_Get ("cl_framerate", "0", CVAR_TEMP);

	// init autoswitch so the ui will have it correctly even
	// if the cgame hasn't been started
	Cvar_Get ("cg_autoswitch", "1", CVAR_ARCHIVE);

	m_pitch = Cvar_Get ("m_pitch", "0.022", CVAR_ARCHIVE_ND);
	m_yaw = Cvar_Get ("m_yaw", "0.022", CVAR_ARCHIVE_ND);
	m_forward = Cvar_Get ("m_forward", "0.25", CVAR_ARCHIVE_ND);
	m_side = Cvar_Get ("m_side", "0.25", CVAR_ARCHIVE_ND);
	m_filter = Cvar_Get ("m_filter", "0", CVAR_ARCHIVE_ND);

	// ~ and `, as keys and characters
	cl_consoleKeys = Cvar_Get( "cl_consoleKeys", "~ ` 0x7e 0x60 0xb2", CVAR_ARCHIVE);
	cl_consoleUseScanCode = Cvar_Get( "cl_consoleUseScanCode", "1", CVAR_ARCHIVE );
	cl_consoleShiftRequirement = Cvar_Get( "cl_consoleShiftRequirement", "0", CVAR_ARCHIVE );

	// userinfo
#ifdef JK2_MODE
	Cvar_Get ("name", "Kyle", CVAR_USERINFO | CVAR_ARCHIVE_ND );
#else
	Cvar_Get ("name", "Jaden", CVAR_USERINFO | CVAR_ARCHIVE_ND );
#endif

#ifdef JK2_MODE
	// this is required for savegame compatibility - not ever actually used
	Cvar_Get ("snaps", "20", CVAR_USERINFO );
	Cvar_Get ("sex", "male", CVAR_USERINFO | CVAR_ARCHIVE );
	Cvar_Get ("handicap", "100", CVAR_USERINFO | CVAR_SAVEGAME );
#else
	Cvar_Get ("sex", "f", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("snd", "jaden_fmle", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );//UI_SetSexandSoundForModel changes to match sounds.cfg for model
	Cvar_Get ("handicap", "100", CVAR_USERINFO | CVAR_SAVEGAME | CVAR_NORESTART);
#endif
	// coop: the character/saber choices ride in the userinfo so a joiner spawns on
	// the host with its own look (same defaults as G_InitCvars)
	Cvar_Get ("g_char_model", "jedi_tf", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_char_skin_head", "head_a1", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_char_skin", "", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );	// coop: whole-model skin
	Cvar_Get ("g_char_skin_torso", "torso_a1", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_char_skin_legs", "lower_a1", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_char_color_red", "255", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_char_color_green", "255", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_char_color_blue", "255", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_saber", "single_1", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_saber2", "", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_saber_color", "yellow", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_saber2_color", "yellow", CVAR_USERINFO | CVAR_ARCHIVE | CVAR_SAVEGAME | CVAR_NORESTART );
	Cvar_Get ("g_fighting_style", "0", CVAR_USERINFO | CVAR_ARCHIVE );
	// coop: a stable per-install id so the host recognises a returning joiner
	// (its progression is kept under it), and the lobby "ready" flag
	{
		cvar_t *guid = Cvar_Get( "coop_guid", "", CVAR_USERINFO | CVAR_ARCHIVE );
		if ( !guid->string[0] ) {
			Cvar_Set( "coop_guid", va( "%08x%08x", (unsigned)time( NULL ) ^ ( (unsigned)rand() << 16 ), (unsigned)Sys_Milliseconds() ^ (unsigned)rand() ) );
		}
	}
	Cvar_Get( "coop_ready", "0", CVAR_USERINFO );

	//
	// register our commands
	//
	Cmd_AddCommand ("cmd", CL_ForwardToServer_f);
	Cmd_AddCommand ("configstrings", CL_Configstrings_f);
	Cmd_AddCommand ("clientinfo", CL_Clientinfo_f);
	Cmd_AddCommand ("snd_restart", CL_Snd_Restart_f);
	Cmd_AddCommand ("vid_restart", CL_Vid_Restart_f);
	Cmd_AddCommand ("connect", CL_Connect_f);
	Cmd_AddCommand ("coop_keys", CL_CoopKeys_f);	// coop: where do keys and mouse go right now
	Cmd_AddCommand ("uiclose", Menus_CloseAll);		// coop dev: close every menu (uimenu stacks them)
	Cmd_AddCommand ("coop_key", CL_CoopKey_f);	// coop dev: press a key by name
	Cmd_AddCommand ("coop_mouse", CL_CoopMouse_f);	// coop dev: feed a mouse delta
	Cmd_AddCommand ("disconnect", CL_Disconnect_f);
	Cmd_AddCommand ("localservers", CL_LocalServers_f);	// D2: LAN co-op discovery
	Cmd_AddCommand ("cinematic", CL_PlayCinematic_f);
	Cmd_SetCommandCompletionFunc( "cinematic", CL_CompleteCinematic );
	Cmd_AddCommand ("ingamecinematic", CL_PlayInGameCinematic_f);
	Cmd_AddCommand ("uimenu", CL_GenericMenu_f);
	Cmd_AddCommand ("datapad", CL_DataPad_f);
	Cmd_AddCommand ("endscreendissolve", CL_EndScreenDissolve_f);

	CL_InitRef();

	CL_StartHunkUsers();

	SCR_Init ();

	Cbuf_Execute ();

	Cvar_Set( "cl_running", "1" );

	Com_Printf( "----- Client Initialization Complete -----\n" );
}


/*
===============
CL_Shutdown

===============
*/
void CL_Shutdown( void ) {
	static qboolean recursive = qfalse;

	if ( !com_cl_running || !com_cl_running->integer ) {
		return;
	}

	Com_Printf( "----- CL_Shutdown -----\n" );

	if ( recursive ) {
		Com_Printf( "WARNING: Recursive CL_Shutdown called!\n" );
		return;
	}
	recursive = qtrue;

	CL_ShutdownUI();
	CL_Disconnect();

	S_Shutdown();
	CL_ShutdownRef(qfalse);

	Cmd_RemoveCommand ("cmd");
	Cmd_RemoveCommand ("configstrings");
	Cmd_RemoveCommand ("clientinfo");
	Cmd_RemoveCommand ("snd_restart");
	Cmd_RemoveCommand ("vid_restart");
	Cmd_RemoveCommand ("disconnect");
	Cmd_RemoveCommand ("cinematic");
	Cmd_RemoveCommand ("ingamecinematic");
	Cmd_RemoveCommand ("uimenu");
	Cmd_RemoveCommand ("datapad");
	Cmd_RemoveCommand ("endscreendissolve");

	Cvar_Set( "cl_running", "0" );

	recursive = qfalse;

	memset( &cls, 0, sizeof( cls ) );

	Com_Printf( "-----------------------\n" );
}

