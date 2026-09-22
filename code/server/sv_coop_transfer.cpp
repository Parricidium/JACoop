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

// sv_coop_transfer.cpp -- coop: host -> joiner transfer of player-model pk3s
//
// A joiner that lacks a pk3 the host loaded (custom skins) would show the
// host as a stormtrooper for the whole campaign. The joiner asks for the
// host's offer list after each gamestate ("coopdl_hello"), picks the checksums
// it does not have ("coopdl_need") and the server streams the packs in
// svc_download blocks appended to its snapshots (plus a few ack-driven
// messages between snapshots), one file at a time, 32 blocks of 1 KB in
// flight. The client acknowledges with one cumulative "coopdl_ack" per
// received message. State per client lives in client_t::coopdl; the lobby
// sees it through the "coop_dl" userinfo key (list | 0..99 | ok | err) that
// G_CoopUpdateLobbyList copies into CS_COOP_LOBBY.
//
// Reliable commands, client -> server:
//   coopdl_hello <version>
//   coopdl_need <n> <sum1> ... <sumn>       (hex checksums from the offer)
//   coopdl_ack <file> <block>               (file = index in the need list, block = highest in-order block, -1 = none)
//   coopdl_done | coopdl_fail <code> | coopdl_stop
// Reliable commands, server -> client:
//   coopdl_list <i> <n> <sum>:<size>:<name> ...   (chunk i, n entries in all; "coopdl_list 0 0" = nothing to offer)
//   coopdl_err <text>
// Binary, in a server message after the snapshot:
//   [byte svc_download][short block] (block 0: [long size][string "<sum>:<name>"]) [short len][len bytes]
//   len == 0 is the EOF block.

#include "../server/exe_headers.h"

#include "server.h"

#define COOP_DL_PROTOCOL	1
#define COOP_DL_RESEND_MS	300		// resend the unacknowledged window after this silence
#define COOP_DL_TIMEOUT_MS	30000	// give up on a client that stops acknowledging
#define COOP_DL_LIST_CHUNK	900		// chars per coopdl_list command (SV_SendServerCommand caps at 1022)

cvar_t	*sv_coopTransfer;			// offer and serve the packs
cvar_t	*sv_coopTransferRate;		// KB/s per joiner off the LAN (0 = unlimited)
cvar_t	*sv_coopTransferRateLan;	// KB/s per joiner on the LAN (0 = unlimited)
cvar_t	*sv_coopTransferMsgKB;		// KB of blocks per message (0 = 12 on the LAN, 8 elsewhere)
cvar_t	*sv_coopTransferMaxMB;		// a bigger pk3 is not offered
cvar_t	*sv_coopTransferPending;	// ROM: joiners still listed / downloading (read by the host's lobby menu)

static coopPakInfo_t	sv_coopOffer[MAX_COOP_OFFER];
static int				sv_coopOfferCount;

/*
==================
SV_CoopTransferInit
==================
*/
void SV_CoopTransferInit( void ) {
	sv_coopTransfer = Cvar_Get( "sv_coopTransfer", "1", CVAR_ARCHIVE );
	sv_coopTransferRate = Cvar_Get( "sv_coopTransferRate", "512", CVAR_ARCHIVE );
	sv_coopTransferRateLan = Cvar_Get( "sv_coopTransferRateLan", "0", CVAR_ARCHIVE );
	sv_coopTransferMsgKB = Cvar_Get( "sv_coopTransferMsgKB", "0", 0 );
	sv_coopTransferMaxMB = Cvar_Get( "sv_coopTransferMaxMB", "100", CVAR_ARCHIVE );
	sv_coopTransferPending = Cvar_Get( "sv_coopTransferPending", "0", CVAR_ROM );
}

// private addresses and the loopback count as LAN (NET_IsLANAddress has no body in SP)
static qboolean SV_CoopIsLanClient( const client_t *cl ) {
	const netadr_t *a = &cl->netchan.remoteAddress;

	if ( a->type == NA_LOOPBACK ) {
		return qtrue;
	}
	if ( a->type != NA_IP ) {
		return qfalse;
	}
	if ( a->ip[0] == 127 || a->ip[0] == 10 || ( a->ip[0] == 192 && a->ip[1] == 168 )
		|| ( a->ip[0] == 172 && a->ip[1] >= 16 && a->ip[1] <= 31 ) || ( a->ip[0] == 169 && a->ip[1] == 254 ) ) {
		return qtrue;
	}
	return qfalse;
}

/*
==================
SV_CoopTransferTagUserinfo

Publish the state in the client's userinfo ("coop_dl" key). The game copies it
into the lobby configstring (4th field) and refuses "ready" while it is busy.
==================
*/
void SV_CoopTransferTagUserinfo( client_t *cl ) {
	const char *v = "";

	switch ( cl->coopdl.state ) {
	case CDL_LISTED:	v = "list"; break;
	case CDL_SENDING:	v = va( "%i", cl->coopdl.lastPct ); break;
	case CDL_DONE:		v = "ok"; break;
	case CDL_FAILED:	v = "err"; break;
	default:			break;
	}
	Info_SetValueForKey( cl->userinfo, "coop_dl", v );	// an empty value removes the key
}

static void SV_CoopCloseFile( client_t *cl ) {
	if ( cl->coopdl.file ) {
		FS_FCloseFile( cl->coopdl.file );
		cl->coopdl.file = 0;
	}
}

/*
==================
SV_CoopTransferClose

Drop everything (SV_DropClient, shutdown, timeout).
==================
*/
void SV_CoopTransferClose( client_t *cl ) {
	SV_CoopCloseFile( cl );
	memset( &cl->coopdl, 0, sizeof( cl->coopdl ) );
	cl->coopdl.state = CDL_NONE;
}

static void SV_CoopSetState( client_t *cl, coopDownloadState_t state ) {
	SV_CoopCloseFile( cl );
	cl->coopdl.state = state;
	SV_CoopTransferTagUserinfo( cl );
}

static void SV_CoopFailClient( client_t *cl, const char *why ) {
	Com_Printf( "coop: transfert vers %s abandonne (%s)\n", cl->name, why );
	SV_SendServerCommand( cl, "coopdl_err %s", why );
	SV_CoopSetState( cl, CDL_FAILED );
}

/*
==================
SV_CoopOpenNext

Open need[needIndex]; false (and the client failed) when it cannot be served.
==================
*/
static qboolean SV_CoopOpenNext( client_t *cl ) {
	coopDownload_t *dl = &cl->coopdl;
	int size;

	SV_CoopCloseFile( cl );
	size = FS_CoopOpenOffered( dl->need[dl->needIndex], &dl->file );
	if ( size < 0 || !dl->file ) {
		SV_CoopFailClient( cl, va( "fichier %08x introuvable", dl->need[dl->needIndex] ) );
		return qfalse;
	}
	dl->fileSum = dl->need[dl->needIndex];
	dl->fileSize = size;
	dl->fileCount = 0;
	dl->currentBlock = dl->clientBlock = dl->xmitBlock = 0;
	dl->eof = qfalse;
	dl->sendTime = 0;
	memset( dl->blockSize, 0, sizeof( dl->blockSize ) );
	if ( com_developer->integer ) {
		int i;
		for ( i = 0; i < sv_coopOfferCount; i++ ) {
			if ( sv_coopOffer[i].checksum == dl->fileSum ) {
				Com_Printf( "coop: envoi de %s (%i octets) a %s\n", sv_coopOffer[i].name, size, cl->name );
			}
		}
	}
	return qtrue;
}

static const char *SV_CoopOfferName( int checksum ) {
	int i;

	for ( i = 0; i < sv_coopOfferCount; i++ ) {
		if ( sv_coopOffer[i].checksum == checksum ) {
			return sv_coopOffer[i].name;
		}
	}
	return "unknown.pk3";
}

/*
==================
SV_CoopHello_f

"coopdl_hello <version>": (re)build the offer and send it in chunks.
==================
*/
void SV_CoopHello_f( client_t *cl ) {
	char line[COOP_DL_LIST_CHUNK + 64];
	int chunk = 0, i;

	SV_CoopTransferClose( cl );
	cl->coopdl.helloTime = sv.time;
	cl->coopdl.lastAckTime = Sys_Milliseconds();

	if ( !sv_coopTransfer->integer ) {
		SV_SendServerCommand( cl, "coopdl_list 0 0" );
		Com_Printf( "coop: %s demande les skins, sv_coopTransfer est a 0\n", cl->name );
		return;
	}
	sv_coopOfferCount = FS_CoopEnumOffered( sv_coopOffer, MAX_COOP_OFFER, sv_coopTransferMaxMB->integer * 1024 * 1024 );
	if ( !sv_coopOfferCount ) {
		SV_SendServerCommand( cl, "coopdl_list 0 0" );
		Com_Printf( "coop: %s demande les skins, rien a offrir\n", cl->name );
		return;
	}

	// "coopdl_list <chunk> <total entries> <sum>:<size>:<name>...", as many chunks as needed
	line[0] = '\0';
	for ( i = 0; i < sv_coopOfferCount; i++ ) {
		const char *entry = va( " %08x:%i:%s", sv_coopOffer[i].checksum, sv_coopOffer[i].size, sv_coopOffer[i].name );

		if ( line[0] && strlen( line ) + strlen( entry ) > COOP_DL_LIST_CHUNK ) {
			SV_SendServerCommand( cl, "coopdl_list %i %i%s", ++chunk, sv_coopOfferCount, line );
			line[0] = '\0';
		}
		Q_strcat( line, sizeof( line ), entry );
	}
	if ( line[0] ) {
		SV_SendServerCommand( cl, "coopdl_list %i %i%s", ++chunk, sv_coopOfferCount, line );
	}
	cl->coopdl.state = CDL_LISTED;
	SV_CoopTransferTagUserinfo( cl );
	Com_Printf( "coop: %i pk3 offert(s) a %s\n", sv_coopOfferCount, cl->name );
}

/*
==================
SV_CoopNeed_f

"coopdl_need <n> <sum>...": the client's shopping list, validated against the offer.
==================
*/
void SV_CoopNeed_f( client_t *cl ) {
	coopDownload_t *dl = &cl->coopdl;
	int n, i, j;

	if ( dl->state != CDL_LISTED ) {
		return;
	}
	n = atoi( Cmd_Argv( 1 ) );
	if ( n < 0 || n > MAX_COOP_OFFER || Cmd_Argc() < 2 + n ) {
		SV_CoopFailClient( cl, "liste invalide" );
		return;
	}
	dl->needCount = 0;
	dl->totalBytes = dl->ackedBytes = 0;
	for ( i = 0; i < n; i++ ) {
		unsigned int sum = (unsigned int)strtoul( Cmd_Argv( 2 + i ), NULL, 16 );

		for ( j = 0; j < sv_coopOfferCount; j++ ) {
			if ( (unsigned int)sv_coopOffer[j].checksum == sum ) {
				break;
			}
		}
		if ( j == sv_coopOfferCount ) {
			SV_CoopFailClient( cl, va( "checksum %08x inconnu", sum ) );
			return;
		}
		dl->need[dl->needCount++] = (int)sum;
		dl->totalBytes += sv_coopOffer[j].size;
	}
	dl->needIndex = 0;
	dl->lastPct = 0;
	dl->lastAckTime = Sys_Milliseconds();
	dl->tokens = 0;
	dl->tokenTime = dl->lastAckTime;
	if ( !dl->needCount ) {
		Com_Printf( "coop: %s a deja tous les skins\n", cl->name );
		SV_CoopSetState( cl, CDL_DONE );
		return;
	}
	Com_Printf( "coop: %s demande %i pk3 (%.1f Mo)\n", cl->name, dl->needCount, dl->totalBytes / ( 1024.0f * 1024.0f ) );
	dl->state = CDL_SENDING;
	if ( SV_CoopOpenNext( cl ) ) {
		SV_CoopTransferTagUserinfo( cl );
	}
}

/*
==================
SV_CoopTransferWrite

Append download blocks to a message going to this client, within a byte budget
that keeps the whole message under MAX_MSGLEN (one datagram: no fragmentation
below that size in this engine, and an overflow would throw the snapshot away).
Port of codemp's SV_WriteDownloadToClient with a cumulative ack and a byte
budget instead of a per-snapshot block count.
==================
*/
void SV_CoopTransferWrite( client_t *cl, msg_t *msg ) {
	coopDownload_t *dl = &cl->coopdl;
	const qboolean lan = SV_CoopIsLanClient( cl );
	int budget, written = 0, rateKB, now;

	if ( dl->state != CDL_SENDING || !dl->file ) {
		return;
	}
	budget = sv_coopTransferMsgKB->integer > 0 ? sv_coopTransferMsgKB->integer * 1024 : ( lan ? 12 : 8 ) * 1024;
	if ( budget > msg->maxsize - msg->cursize - 512 ) {
		budget = msg->maxsize - msg->cursize - 512;
	}
	if ( budget < COOP_DL_BLK + 128 ) {
		return;
	}
	now = Sys_Milliseconds();

	// read ahead into the window
	while ( dl->currentBlock - dl->clientBlock < COOP_DL_WINDOW && dl->fileCount < dl->fileSize ) {
		const int idx = dl->currentBlock % COOP_DL_WINDOW;
		int want = dl->fileSize - dl->fileCount;

		if ( want > COOP_DL_BLK ) {
			want = COOP_DL_BLK;
		}
		dl->blockSize[idx] = FS_Read( dl->blocks[idx], want, dl->file );
		if ( dl->blockSize[idx] <= 0 ) {
			dl->fileCount = dl->fileSize;	// short file: finish now
			dl->blockSize[idx] = 0;
			break;
		}
		dl->fileCount += dl->blockSize[idx];
		dl->currentBlock++;
	}
	// queue the EOF block once the file is read through
	if ( dl->fileCount == dl->fileSize && !dl->eof && dl->currentBlock - dl->clientBlock < COOP_DL_WINDOW ) {
		dl->blockSize[dl->currentBlock % COOP_DL_WINDOW] = 0;
		dl->currentBlock++;
		dl->eof = qtrue;
	}

	// token bucket, KB/s
	rateKB = lan ? sv_coopTransferRateLan->integer : sv_coopTransferRate->integer;
	if ( rateKB > 0 ) {
		dl->tokens += (int)( (float)( now - dl->tokenTime ) * rateKB * 1024 / 1000.0f );
		if ( dl->tokens > rateKB * 1024 ) {
			dl->tokens = rateKB * 1024;
		}
	}
	dl->tokenTime = now;

	while ( 1 ) {
		int idx, size, need;
		const char *header = NULL;

		if ( dl->xmitBlock == dl->currentBlock ) {
			// the whole window went out; resend it when the client stays silent
			if ( dl->clientBlock == dl->currentBlock ) {
				break;	// nothing outstanding
			}
			if ( now - dl->sendTime <= COOP_DL_RESEND_MS ) {
				break;
			}
			dl->xmitBlock = dl->clientBlock;
			if ( com_developer->integer ) {
				Com_Printf( "coop: renvoi des blocs %i-%i a %s\n", dl->clientBlock, dl->currentBlock - 1, cl->name );
			}
		}
		idx = dl->xmitBlock % COOP_DL_WINDOW;
		size = dl->blockSize[idx];
		need = 1 + 2 + 2 + size;
		if ( dl->xmitBlock == 0 ) {
			header = va( "%08x:%s", dl->fileSum, SV_CoopOfferName( dl->fileSum ) );
			need += 4 + (int)strlen( header ) + 1;
		}
		if ( written + need > budget ) {
			break;
		}
		if ( rateKB > 0 && size > 0 ) {
			if ( dl->tokens < size ) {
				break;
			}
			dl->tokens -= size;
		}
		MSG_WriteByte( msg, svc_download );
		MSG_WriteShort( msg, dl->xmitBlock & 0xffff );
		if ( header ) {
			MSG_WriteLong( msg, dl->fileSize );
			MSG_WriteString( msg, header );
		}
		MSG_WriteShort( msg, size );
		if ( size ) {
			MSG_WriteData( msg, dl->blocks[idx], size );
		}
		written += need;
		dl->xmitBlock++;
		dl->sendTime = now;
	}
}

/*
==================
SV_CoopTransferSendEager

A message of blocks only (plus pending reliable commands), sent right away in
reaction to an ack so the LAN does not wait for the next snapshot. Capped per
server frame (each one burns a netchan sequence, see SV_WriteSnapshotToClient).
==================
*/
static void SV_CoopTransferSendEager( client_t *cl ) {
	byte	msg_buf[MAX_MSGLEN];
	msg_t	msg;
	coopDownload_t *dl = &cl->coopdl;

	if ( cl->state != CS_ACTIVE || dl->state != CDL_SENDING || !dl->file || dl->eagerThisFrame >= COOP_DL_EAGER_MAX ) {
		return;
	}
	if ( dl->xmitBlock == dl->currentBlock && dl->fileCount == dl->fileSize && dl->eof ) {
		return;	// everything is out, wait for acks (or the resend timer in the snapshot path)
	}
	MSG_Init( &msg, msg_buf, sizeof( msg_buf ) );
	msg.allowoverflow = qtrue;
	SV_UpdateServerCommandsToClient( cl, &msg );
	SV_CoopTransferWrite( cl, &msg );
	if ( msg.overflowed ) {
		Com_Printf( "WARNING: coop transfer msg overflowed for %s\n", cl->name );
		return;
	}
	if ( !msg.cursize ) {
		return;
	}
	dl->eagerThisFrame++;
	SV_SendMessageToClient( &msg, cl );
}

/*
==================
SV_CoopAck_f

"coopdl_ack <file> <block>": cumulative; the client got every block up to
'block' of file number 'file' (its index in the need list). A repeated value
means a message was lost: resend at once. An ack for another file is stale.
==================
*/
void SV_CoopAck_f( client_t *cl ) {
	coopDownload_t *dl = &cl->coopdl;
	int block, b;

	if ( dl->state != CDL_SENDING || !dl->file ) {
		return;
	}
	dl->lastAckTime = Sys_Milliseconds();
	if ( atoi( Cmd_Argv( 1 ) ) != dl->needIndex ) {
		return;
	}
	block = atoi( Cmd_Argv( 2 ) );
	if ( block >= dl->currentBlock ) {
		return;	// acknowledging what was never sent
	}
	if ( block >= dl->clientBlock ) {
		for ( b = dl->clientBlock; b <= block; b++ ) {
			dl->ackedBytes += dl->blockSize[b % COOP_DL_WINDOW];
		}
		dl->clientBlock = block + 1;
		if ( dl->eof && dl->clientBlock == dl->currentBlock ) {
			// the EOF block is acknowledged: this file is complete
			if ( com_developer->integer ) {
				Com_Printf( "coop: %s a recu %s\n", cl->name, SV_CoopOfferName( dl->fileSum ) );
			}
			SV_CoopCloseFile( cl );
			dl->needIndex++;
			if ( dl->needIndex < dl->needCount ) {
				if ( !SV_CoopOpenNext( cl ) ) {
					return;
				}
			} else {
				return;	// the client verifies and loads, then says coopdl_done
			}
		}
	} else if ( block == dl->clientBlock - 1 && dl->xmitBlock > dl->clientBlock ) {
		// duplicate ack: the client saw a message without the block it waits for
		dl->xmitBlock = dl->clientBlock;
		if ( com_developer->integer ) {
			Com_Printf( "coop: ack en double (%i), renvoi a partir du bloc %i a %s\n", block, dl->clientBlock, cl->name );
		}
	}
	SV_CoopTransferSendEager( cl );
}

/*
==================
SV_CoopDone_f / SV_CoopFail_f / SV_CoopStop_f
==================
*/
void SV_CoopDone_f( client_t *cl ) {
	coopDownload_t *dl = &cl->coopdl;

	if ( dl->state != CDL_SENDING && dl->state != CDL_LISTED ) {
		return;
	}
	Com_Printf( "coop: %s a recu et charge les skins (%i fichier(s), %.1f Mo)\n", cl->name, dl->needCount, dl->totalBytes / ( 1024.0f * 1024.0f ) );
	SV_CoopSetState( cl, CDL_DONE );
}

void SV_CoopFail_f( client_t *cl ) {
	if ( cl->coopdl.state != CDL_SENDING && cl->coopdl.state != CDL_LISTED ) {
		return;
	}
	Com_Printf( "coop: %s a abandonne le transfert (%s)\n", cl->name, Cmd_Argv( 1 ) );
	SV_CoopSetState( cl, CDL_FAILED );
}

void SV_CoopStop_f( client_t *cl ) {
	if ( cl->coopdl.state != CDL_SENDING && cl->coopdl.state != CDL_LISTED ) {
		return;
	}
	Com_Printf( "coop: %s a arrete le transfert\n", cl->name );
	SV_CoopSetState( cl, CDL_FAILED );
}

/*
==================
SV_CoopTransferFrame

Once per server frame, before the snapshots go out: reset the eager counter,
time out silent clients, refresh the progress tag and sv_coopTransferPending.
==================
*/
void SV_CoopTransferFrame( void ) {
	int i, pending = 0;
	const int now = Sys_Milliseconds();

	if ( !svs.clients ) {
		return;
	}
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		client_t *cl = &svs.clients[i];
		coopDownload_t *dl = &cl->coopdl;

		dl->eagerThisFrame = 0;
		if ( cl->state < CS_CONNECTED ) {
			continue;
		}
		if ( dl->state != CDL_LISTED && dl->state != CDL_SENDING ) {
			continue;
		}
		if ( !sv_coopTransfer->integer ) {
			SV_CoopFailClient( cl, "sv_coopTransfer 0" );
			continue;
		}
		if ( now - dl->lastAckTime > COOP_DL_TIMEOUT_MS ) {
			SV_CoopFailClient( cl, "timeout" );
			continue;
		}
		if ( dl->state == CDL_SENDING && dl->totalBytes > 0 ) {
			int pct = (int)( (float)dl->ackedBytes * 100.0f / (float)dl->totalBytes );

			if ( pct > 99 ) {
				pct = 99;
			}
			if ( pct / 5 != dl->lastPct / 5 ) {
				dl->lastPct = pct;
				SV_CoopTransferTagUserinfo( cl );
			}
		}
		pending++;
	}
	if ( sv_coopTransferPending->integer != pending ) {
		Cvar_Set( "sv_coopTransferPending", va( "%i", pending ) );
	}
}

/*
==================
SV_CoopTransferShutdown

Server going down: close the pak handles before svs.clients is freed.
==================
*/
void SV_CoopTransferShutdown( void ) {
	int i;

	if ( svs.clients ) {
		for ( i = 0; i < MAX_CLIENTS; i++ ) {
			SV_CoopTransferClose( &svs.clients[i] );
		}
	}
	if ( sv_coopTransferPending ) {
		Cvar_Set( "sv_coopTransferPending", "0" );
	}
}

/*
==================
SV_CoopTransferBlocksMapChange

Console guard: in the lobby, "map" / "load" wait for the joiners' downloads
(the menu buttons are hidden by the UI, this catches the console path).
==================
*/
qboolean SV_CoopTransferBlocksMapChange( const char *what ) {
	if ( !sv_coopTransferPending || sv_coopTransferPending->integer <= 0 ) {
		return qfalse;
	}
	if ( !Cvar_VariableIntegerValue( "g_coopLobby" ) ) {
		return qfalse;
	}
	Com_Printf( S_COLOR_YELLOW "coop: transfert de skins en cours vers %i joueur(s), attends la fin avant '%s' (ou sv_coopTransfer 0)\n", sv_coopTransferPending->integer, what );
	return qtrue;
}
