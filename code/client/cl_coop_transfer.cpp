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

// cl_coop_transfer.cpp -- coop: joiner side of the host -> joiner pk3 transfer
//
// After each gamestate a remote client says "coopdl_hello"; the host answers
// with its offer ("coopdl_list", checksums + sizes + names of the player-model
// pk3s it loaded). Whatever we do not have (FS_CoopHavePak) is requested with
// "coopdl_need" and arrives as svc_download blocks (see sv_coop_transfer.cpp),
// written to <fs_homepath>/base/coopdl_<sum>_<name>.tmp, renamed and loaded
// into the search path without a restart once complete and verified. One
// cumulative "coopdl_ack <file> <block>" per received message drives the
// server's window. When the last file is in, we go ready (coop_ready 1) by
// ourselves, the renderer forgets its failed lookups and the cgame rebuilds
// the players' models (cl_coopPaksGen). The menus read cl_coopTransferState /
// cl_coopTransferStatus / cl_coopTransferPct.

#include "../server/exe_headers.h"

#include "client.h"

#define COOP_DL_PROTOCOL		1
#define COOP_DL_LIST_TIMEOUT	10000	// ms without an answer to the hello
#define COOP_DL_BLOCK_TIMEOUT	30000	// ms without a block

cvar_t	*cl_coopTransfer;			// ask the host for the pk3s we lack
cvar_t	*cl_coopTransferMaxMB;		// total accepted per session
cvar_t	*cl_coopUploadKB;			// KB of our own mods per packet to the host
cvar_t	*cl_coopTransferState;		// ROM: "" | list | downloading | done | error
cvar_t	*cl_coopTransferStatus;		// ROM: text for the lobby
cvar_t	*cl_coopTransferPct;		// ROM
cvar_t	*cl_coopPaksGen;			// ROM: bumped by files.cpp at each pack add / unload
cvar_t	*cl_coopUpload;				// coop: offer our own mods to the host (the other direction)

static void CL_CoopUploadStart( void );

/*
==================
CL_CoopTransferInit
==================
*/
void CL_CoopTransferInit( void ) {
	cl_coopTransfer = Cvar_Get( "cl_coopTransfer", "1", CVAR_ARCHIVE );
	cl_coopTransferMaxMB = Cvar_Get( "cl_coopTransferMaxMB", "300", CVAR_ARCHIVE );
	// KB of our own mods per packet we send the host (1 = one datagram, no
	// fragmenting; 32 = the most we allow)
	cl_coopUploadKB = Cvar_Get( "cl_coopUploadKB", "8", CVAR_ARCHIVE );
	cl_coopTransferState = Cvar_Get( "cl_coopTransferState", "", CVAR_ROM );
	cl_coopTransferStatus = Cvar_Get( "cl_coopTransferStatus", "", CVAR_ROM );
	cl_coopTransferPct = Cvar_Get( "cl_coopTransferPct", "0", CVAR_ROM );
	cl_coopPaksGen = Cvar_Get( "cl_coopPaksGen", "0", CVAR_ROM );
	cl_coopUpload = Cvar_Get( "cl_coopUpload", "1", CVAR_ARCHIVE );
	Cvar_Set( "cl_coopTransferState", "" );
	Cvar_Set( "cl_coopTransferStatus", "" );
	Cvar_Set( "cl_coopTransferPct", "0" );
}

/*
==================
CL_CoopCheckPaksGen

The filesystem bumps cl_coopPaksGen every time a coop pack enters or leaves the
search path: a finished download, a leftover coopdl_*.pk3 loaded again by
FS_CoopHavePak (a reconnection, where nothing is downloaded at all), an unload
at disconnect. In every one of those cases the renderer is still holding the
failed lookups it made while the pack was missing - the model cached as MOD_BAD,
the skin as a zero-surface entry RE_RegisterSkin answers 0 for ever after, the
shaders as default ones - so drop them here rather than only on the download
path. Called from CL_CoopTransferFrame, i.e. in CL_Frame before anything is
drawn, so the cgame's own generation check (which registers cgs.skins again and
rebuilds the characters) always runs after this.
==================
*/
void CL_CoopCheckPaksGen( void ) {
	static int	paksGenSeen = -1;
	const int	gen = cl_coopPaksGen ? cl_coopPaksGen->integer : 0;

	if ( paksGenSeen == gen ) {
		return;
	}
	if ( paksGenSeen == -1 ) {
		paksGenSeen = gen;	// first look: nothing has been registered yet
		return;
	}
	if ( !cls.rendererStarted || !re.CoopForgetMissing ) {
		return;				// try again once the renderer is up
	}
	paksGenSeen = gen;
	re.CoopForgetMissing();
}

static void CL_CoopSetState( clientCoopDownloadState_t state, const char *cvarState ) {
	clc.coopdl.state = state;
	Cvar_Set( "cl_coopTransferState", cvarState );
}

static void CL_CoopSetStatus( const char *text ) {
	Cvar_Set( "cl_coopTransferStatus", text );
}

static void CL_CoopCloseCurrent( qboolean removeTmp ) {
	if ( clc.coopdl.cur.file ) {
		FS_FCloseFile( clc.coopdl.cur.file );
		clc.coopdl.cur.file = 0;
	}
	if ( removeTmp && clc.coopdl.cur.relTmp[0] ) {
		FS_CoopRemoveFile( clc.coopdl.cur.relTmp );
	}
	clc.coopdl.cur.relTmp[0] = '\0';
}

/*
=============================================================================

coop: our own mods, pushed to the host

A joiner may be the one with the custom character or the custom hilt. The host
cannot ask us for a file (in this engine the server talks first and a client has
no svc_download), so once our download from the host is settled we announce
what we could give ("coopup_list"), the host answers with the checksums it does
not have ("coopup_want"), and we push those files inside our ordinary command
packets with the new clc_coopUpload opcode (CL_CoopUploadWritePacket, called at
the end of CL_WritePacket).

A client packet must stay under MAX_PACKETLEN or the netchan would fragment it,
so a block is COOP_UP_BLK (768 B) and only what is left of the packet is used:
at ~20-30 packets a second that is 15-25 KB/s, enough for a skin or hilt pack.
The host acks cumulatively ("coopup_ack <file> <block>") once per packet that
carried blocks, exactly like we do for its downloads, and we resend the window
when it stays silent.

The host writes what it receives as coopup_<sum>_<name>.pk3, checks it against
the same whitelist and offers it to the other joiners in turn.
=============================================================================
*/

#define COOP_UP_RESEND_MS		300		// resend the unacknowledged window after this silence
#define COOP_UP_WANT_TIMEOUT	10000	// ms without an answer to the announce
#define COOP_UP_ACK_TIMEOUT		30000	// ms without an ack

static void CL_CoopUploadCloseFile( void ) {
	if ( clc.coopup.file ) {
		FS_FCloseFile( clc.coopup.file );
		clc.coopup.file = 0;
	}
}

static void CL_CoopUploadFail( const char *code ) {
	Com_Printf( S_COLOR_YELLOW "coop: envoi de nos mods echoue (%s)\n", code );
	CL_CoopUploadCloseFile();
	if ( cls.state >= CA_CONNECTED ) {
		CL_AddReliableCommand( va( "coopup_fail %s", code ) );
	}
	clc.coopup.state = CLUP_FAILED;
	Cvar_Set( "cl_coopTransferState", "error" );
	CL_CoopSetStatus( va( "Echec de l'envoi de mes mods a l'hote (%s).", code ) );
}

static void CL_CoopUploadDone( void ) {
	clientCoopUpload_t *up = &clc.coopup;
	const float secs = ( cls.realtime - up->startTime ) / 1000.0f;

	CL_CoopUploadCloseFile();
	up->state = CLUP_DONE;
	Cvar_Set( "cl_coopTransferState", "done" );
	CL_AddReliableCommand( "coopup_done" );
	if ( up->sendCount > 0 ) {
		Com_Printf( "coop: mes mods envoyes a l'hote (%i fichier(s), %.1f Mo, %.1f s, %.0f Ko/s)\n",
			up->sendCount, up->totalBytes / ( 1024.0f * 1024.0f ), secs,
			secs > 0.01f ? up->totalBytes / 1024.0f / secs : 0.0f );
		CL_CoopSetStatus( va( "Mes mods envoyes a l'hote (%i fichier(s), %.1f Mo).", up->sendCount, up->totalBytes / ( 1024.0f * 1024.0f ) ) );
	}
}

/*
==================
CL_CoopUploadOpenNext

Open send[sendIndex] for reading, by checksum only.
==================
*/
static qboolean CL_CoopUploadOpenNext( void ) {
	clientCoopUpload_t *up = &clc.coopup;
	int size, i;

	CL_CoopUploadCloseFile();
	size = FS_CoopOpenOffered( up->send[up->sendIndex], &up->file );
	if ( size < 0 || !up->file ) {
		CL_CoopUploadFail( "introuvable" );
		return qfalse;
	}
	up->curSum = up->send[up->sendIndex];
	up->curSize = size;
	up->curCount = 0;
	up->curName[0] = '\0';
	for ( i = 0; i < up->mineCount; i++ ) {
		if ( up->mine[i].checksum == up->curSum ) {
			Q_strncpyz( up->curName, up->mine[i].name, sizeof( up->curName ) );
		}
	}
	up->currentBlock = up->hostBlock = up->xmitBlock = 0;
	up->eof = qfalse;
	up->sendTime = 0;
	memset( up->blockSize, 0, sizeof( up->blockSize ) );
	Com_Printf( "coop: envoi de %s a l'hote (%.1f Mo)\n", up->curName, size / ( 1024.0f * 1024.0f ) );
	return qtrue;
}

/*
==================
CL_CoopUploadStart

Our download from the host is settled: announce what we have that the host may
not. Nothing to announce (or cl_coopUpload 0, or we ARE the host) = nothing to do.
==================
*/
static void CL_CoopUploadStart( void ) {
	clientCoopUpload_t *up = &clc.coopup;
	char line[1024], cmd[MAX_STRING_CHARS];
	int chunk = 0, i;

	if ( com_sv_running->integer || !cl_coopUpload || !cl_coopUpload->integer ) {
		return;
	}
	if ( up->state != CLUP_IDLE || cls.state < CA_CONNECTED ) {
		return;
	}
	up->mineCount = FS_CoopEnumOffered( up->mine, MAX_COOP_OFFER, 0 );
	if ( !up->mineCount ) {
		return;
	}
	// "coopup_list <chunk> <total> <sum>:<size>:<name>...", as many chunks as needed
	line[0] = '\0';
	for ( i = 0; i < up->mineCount; i++ ) {
		const char *entry = va( " %08x:%i:%s", up->mine[i].checksum, up->mine[i].size, up->mine[i].name );

		if ( line[0] && strlen( line ) + strlen( entry ) > 800 ) {
			Com_sprintf( cmd, sizeof( cmd ), "coopup_list %i %i%s", ++chunk, up->mineCount, line );
			CL_AddReliableCommand( cmd );
			line[0] = '\0';
		}
		Q_strcat( line, sizeof( line ), entry );
	}
	if ( line[0] ) {
		Com_sprintf( cmd, sizeof( cmd ), "coopup_list %i %i%s", ++chunk, up->mineCount, line );
		CL_AddReliableCommand( cmd );
	}
	up->state = CLUP_WAITWANT;
	up->announceTime = cls.realtime;
	Com_Printf( "coop: %i mod(s) a nous proposes a l'hote\n", up->mineCount );
}

/*
==================
CL_CoopUploadServerCommand

"coopup_want <n> <sum>...", "coopup_ack <file> <block>" and "coopup_err <text>",
already split into argv by CL_CoopTransferServerCommand.
==================
*/
static void CL_CoopUploadServerCommand( int argc, char **argv, const char *rest ) {
	clientCoopUpload_t *up = &clc.coopup;

	if ( !strcmp( argv[0], "coopup_want" ) ) {
		int n, i;

		if ( up->state != CLUP_WAITWANT || argc < 2 ) {
			return;
		}
		n = atoi( argv[1] );
		if ( n > MAX_COOP_OFFER ) {
			n = MAX_COOP_OFFER;
		}
		up->sendCount = 0;
		up->totalBytes = up->ackedBytes = 0;
		for ( i = 0; i < n && 2 + i < argc; i++ ) {
			const int sum = (int)strtoul( argv[2 + i], NULL, 16 );
			int j;

			for ( j = 0; j < up->mineCount; j++ ) {
				if ( up->mine[j].checksum == sum ) {
					break;
				}
			}
			if ( j == up->mineCount ) {
				continue;	// not something we offered
			}
			up->send[up->sendCount++] = sum;
			up->totalBytes += up->mine[j].size;
		}
		if ( !up->sendCount ) {
			Com_Printf( "coop: l'hote a deja tous nos mods\n" );
			up->state = CLUP_DONE;
			return;
		}
		Com_Printf( "coop: l'hote veut %i de nos mods (%.1f Mo)\n", up->sendCount, up->totalBytes / ( 1024.0f * 1024.0f ) );
		up->sendIndex = 0;
		up->startTime = up->lastAckTime = cls.realtime;
		up->state = CLUP_SENDING;
		Cvar_Set( "cl_coopTransferState", "uploading" );	// the lobby hides "JE SUIS PRET"
		if ( !CL_CoopUploadOpenNext() ) {
			return;
		}
		CL_CoopSetStatus( va( "Envoi de mes mods a l'hote (%i fichier(s), %.1f Mo)...", up->sendCount, up->totalBytes / ( 1024.0f * 1024.0f ) ) );
		return;
	}

	if ( !strcmp( argv[0], "coopup_ack" ) ) {
		int block, b;

		if ( up->state != CLUP_SENDING || !up->file || argc < 3 ) {
			return;
		}
		up->lastAckTime = cls.realtime;
		if ( atoi( argv[1] ) != up->sendIndex ) {
			return;	// stale, for the previous file
		}
		block = atoi( argv[2] );
		if ( block >= up->currentBlock ) {
			return;	// acknowledging what was never sent
		}
		if ( block >= up->hostBlock ) {
			for ( b = up->hostBlock; b <= block; b++ ) {
				up->ackedBytes += up->blockSize[b % COOP_UP_WINDOW];
			}
			up->hostBlock = block + 1;
			if ( up->eof && up->hostBlock == up->currentBlock ) {
				// the EOF block is acknowledged: this file is in
				Com_Printf( "coop: %s envoye\n", up->curName );
				CL_CoopUploadCloseFile();
				up->sendIndex++;
				if ( up->sendIndex < up->sendCount ) {
					CL_CoopUploadOpenNext();
				} else {
					CL_CoopUploadDone();
				}
			}
		} else if ( block == up->hostBlock - 1 && up->xmitBlock > up->hostBlock ) {
			up->xmitBlock = up->hostBlock;	// duplicate ack: resend from there
		}
		return;
	}

	if ( !strcmp( argv[0], "coopup_err" ) ) {
		if ( up->state == CLUP_WAITWANT || up->state == CLUP_SENDING ) {
			Com_Printf( S_COLOR_YELLOW "coop: l'hote a refuse nos mods (%s)\n", rest ? rest : "" );
			CL_CoopUploadCloseFile();
			up->state = CLUP_FAILED;
			Cvar_Set( "cl_coopTransferState", "error" );
		}
		return;
	}
}

/*
==================
CL_CoopUploadWritePacket

End of CL_WritePacket: put as many blocks as cl_coopUploadKB allows into the
packet. One block per packet (all that fits in a single datagram) is only some
40 KB/s, so the packet is allowed to grow and be fragmented by the netchan.
==================
*/
void CL_CoopUploadWritePacket( msg_t *msg ) {
	clientCoopUpload_t *up = &clc.coopup;
	int budget, kb;

	if ( up->state != CLUP_SENDING || !up->file || cls.state < CA_CONNECTED ) {
		return;
	}
	kb = cl_coopUploadKB ? cl_coopUploadKB->integer : 8;
	if ( kb < 1 ) {
		kb = 1;		// one datagram's worth, no fragmenting
	} else if ( kb > 32 ) {
		kb = 32;
	}
	budget = kb * 1024;
	if ( budget > msg->maxsize - msg->cursize - 512 ) {
		budget = msg->maxsize - msg->cursize - 512;
	}
	if ( budget < COOP_UP_BLK + 64 ) {
		return;
	}

	// read ahead into the window
	while ( up->currentBlock - up->hostBlock < COOP_UP_WINDOW && up->curCount < up->curSize ) {
		const int idx = up->currentBlock % COOP_UP_WINDOW;
		int want = up->curSize - up->curCount;

		if ( want > COOP_UP_BLK ) {
			want = COOP_UP_BLK;
		}
		up->blockSize[idx] = FS_Read( up->blocks[idx], want, up->file );
		if ( up->blockSize[idx] <= 0 ) {
			up->curCount = up->curSize;	// short file: finish now
			up->blockSize[idx] = 0;
			break;
		}
		up->curCount += up->blockSize[idx];
		up->currentBlock++;
	}
	// queue the EOF block once the file is read through
	if ( up->curCount == up->curSize && !up->eof && up->currentBlock - up->hostBlock < COOP_UP_WINDOW ) {
		up->blockSize[up->currentBlock % COOP_UP_WINDOW] = 0;
		up->currentBlock++;
		up->eof = qtrue;
	}

	while ( 1 ) {
		int idx, size, need;
		const char *header = NULL;

		if ( up->xmitBlock == up->currentBlock ) {
			// the whole window went out; resend it when the host stays silent
			if ( up->hostBlock == up->currentBlock ) {
				break;	// nothing outstanding
			}
			if ( cls.realtime - up->sendTime <= COOP_UP_RESEND_MS ) {
				break;
			}
			up->xmitBlock = up->hostBlock;
		}
		idx = up->xmitBlock % COOP_UP_WINDOW;
		size = up->blockSize[idx];
		need = 1 + 4 + 2 + size;
		if ( up->xmitBlock == 0 ) {
			header = va( "%08x:%s", up->curSum, up->curName );
			need += 4 + (int)strlen( header ) + 1;
		}
		if ( need > budget ) {
			break;
		}
		MSG_WriteByte( msg, clc_coopUpload );
		MSG_WriteLong( msg, up->xmitBlock );
		if ( header ) {
			MSG_WriteLong( msg, up->curSize );
			MSG_WriteString( msg, header );
		}
		MSG_WriteShort( msg, size );
		if ( size ) {
			MSG_WriteData( msg, up->blocks[idx], size );
		}
		budget -= need;
		up->xmitBlock++;
		up->sendTime = cls.realtime;
	}
}

/*
==================
CL_CoopUploadFrame

Timeouts and the lobby's status line, from CL_CoopTransferFrame.
==================
*/
static void CL_CoopUploadFrame( void ) {
	clientCoopUpload_t *up = &clc.coopup;

	if ( up->state == CLUP_WAITWANT ) {
		if ( cls.realtime - up->announceTime > COOP_UP_WANT_TIMEOUT ) {
			Com_Printf( "coop: l'hote ne repond pas sur nos mods, on continue sans\n" );
			up->state = CLUP_DONE;	// an older host: do not ask again this connection
		}
		return;
	}
	if ( up->state != CLUP_SENDING ) {
		return;
	}
	if ( cls.realtime - up->lastAckTime > COOP_UP_ACK_TIMEOUT ) {
		CL_CoopUploadFail( "timeout" );
		return;
	}
	if ( Cvar_VariableIntegerValue( "coop_ready" ) ) {
		Cvar_Set( "coop_ready", "0" );	// not while a file of ours is still going up
	}
	if ( up->totalBytes > 0 ) {
		int pct = (int)( (float)up->ackedBytes * 100.0f / (float)up->totalBytes );
		const float secs = ( cls.realtime - up->startTime ) / 1000.0f;

		if ( pct > 99 ) {
			pct = 99;
		}
		CL_CoopSetStatus( va( "Envoi de %s a l'hote : %i %%  (%.0f Ko/s)", up->curName[0] ? up->curName : "...", pct,
			secs > 0.5f ? up->ackedBytes / 1024.0f / secs : 0.0f ) );
	}
}

/*
==================
CL_CoopUploadAbort

CL_Disconnect: stop sending, the state goes with the connection.
==================
*/
static void CL_CoopUploadAbort( void ) {
	CL_CoopUploadCloseFile();
	memset( &clc.coopup, 0, sizeof( clc.coopup ) );
	clc.coopup.state = CLUP_IDLE;
}

/*
==================
CL_CoopTransferFail

Give up: tell the host (code: size, disk, checksum, content, timeout), drop the
partial file, keep what was already loaded.
==================
*/
static void CL_CoopTransferFail( const char *code, const char *detail ) {
	Com_Printf( S_COLOR_YELLOW "coop: transfert echoue (%s%s%s)\n", code, detail && detail[0] ? ": " : "", detail ? detail : "" );
	CL_CoopCloseCurrent( qtrue );
	if ( cls.state >= CA_CONNECTED ) {
		CL_AddReliableCommand( va( "coopdl_fail %s", code ) );
	}
	CL_CoopSetState( CLDL_FAILED, "error" );
	CL_CoopSetStatus( va( "Echec du telechargement des skins de l'hote (%s).", code ) );
	Cvar_Set( "cl_coopTransferPct", "0" );
}

/*
==================
CL_CoopTransferHello

End of CL_ParseGamestate: a remote client asks the host what it offers. Once
per connection (the state survives gamestates and is wiped at disconnect).
==================
*/
void CL_CoopTransferHello( void ) {
	if ( com_sv_running->integer || !cl_coopTransfer || !cl_coopTransfer->integer ) {
		return;
	}
	if ( clc.coopdl.state != CLDL_IDLE ) {
		return;
	}
	CL_AddReliableCommand( va( "coopdl_hello %i", COOP_DL_PROTOCOL ) );
	clc.coopdl.helloTime = cls.realtime;
	CL_CoopSetState( CLDL_WAITLIST, "list" );
	CL_CoopSetStatus( "Verification des skins de l'hote..." );
	Com_DPrintf( "coop: demande de la liste des skins de l'hote\n" );
}

/*
==================
CL_CoopOfferByChecksum
==================
*/
static const coopPakInfo_t *CL_CoopOfferByChecksum( int sum ) {
	int i;

	for ( i = 0; i < clc.coopdl.offerCount; i++ ) {
		if ( clc.coopdl.offer[i].checksum == sum ) {
			return &clc.coopdl.offer[i];
		}
	}
	return NULL;
}

/*
==================
CL_CoopTransferListComplete

The whole offer is in: keep what we lack, ask for it (or report done).
==================
*/
static void CL_CoopTransferListComplete( void ) {
	clientCoopDownload_t *dl = &clc.coopdl;
	char cmd[MAX_STRING_CHARS];
	int i, maxBytes = cl_coopTransferMaxMB->integer * 1024 * 1024;

	dl->needCount = 0;
	dl->totalBytes = dl->doneBytes = 0;
	for ( i = 0; i < dl->offerCount; i++ ) {
		if ( FS_CoopHavePak( dl->offer[i].checksum ) ) {
			continue;
		}
		if ( maxBytes > 0 && dl->totalBytes + dl->offer[i].size > maxBytes ) {
			// coop: skip that one and take the rest (cl_coopTransferMaxMB is a total)
			Com_Printf( S_COLOR_YELLOW "coop: %s ignore, cl_coopTransferMaxMB (%i Mo) serait depasse\n",
				dl->offer[i].name, cl_coopTransferMaxMB->integer );
			continue;
		}
		dl->need[dl->needCount++] = dl->offer[i].checksum;
		dl->totalBytes += dl->offer[i].size;
	}
	Com_Printf( "coop: liste de l'hote recue : %i pk3, %i manquant(s) (%.1f Mo)\n", dl->offerCount, dl->needCount, dl->totalBytes / ( 1024.0f * 1024.0f ) );
	if ( !dl->needCount ) {
		// nothing to fetch: no auto-ready, the player still picks its character
		CL_AddReliableCommand( "coopdl_done" );
		CL_CoopSetState( CLDL_DONE, "done" );
		CL_CoopSetStatus( "" );
		CL_CoopUploadStart();	// coop: ... but WE may have a mod the host lacks
		return;
	}
	Com_sprintf( cmd, sizeof( cmd ), "coopdl_need %i", dl->needCount );
	for ( i = 0; i < dl->needCount; i++ ) {
		Q_strcat( cmd, sizeof( cmd ), va( " %08x", dl->need[i] ) );
	}
	CL_AddReliableCommand( cmd );
	dl->needIndex = 0;
	dl->downloadedFiles = 0;
	dl->startTime = dl->lastBlockTime = dl->lastTickTime = cls.realtime;
	dl->bytesAtLastTick = 0;
	dl->speed = 0;
	CL_CoopSetState( CLDL_RECEIVING, "downloading" );
	CL_CoopSetStatus( va( "Telechargement des skins de l'hote (%i fichier(s), %.1f Mo)...", dl->needCount, dl->totalBytes / ( 1024.0f * 1024.0f ) ) );
	Cvar_Set( "cl_coopTransferPct", "0" );
}

/*
==================
CL_CoopTransferServerCommand

"coopdl_*" reliable commands, executed at receive time (CL_ParseCommandString);
the cgame never sees them (CL_GetServerCommand). Parsed by hand so the console
tokenizer is left alone.
==================
*/
void CL_CoopTransferServerCommand( const char *s ) {
	clientCoopDownload_t *dl = &clc.coopdl;
	char buf[MAX_STRING_CHARS];
	char *argv[MAX_COOP_OFFER + 8];
	int argc = 0;
	char *p;

	Q_strncpyz( buf, s, sizeof( buf ) );
	for ( p = buf; *p && argc < (int)ARRAY_LEN( argv ); ) {
		while ( *p && *(unsigned char *)p <= ' ' ) {
			p++;
		}
		if ( !*p ) {
			break;
		}
		argv[argc++] = p;
		while ( *(unsigned char *)p > ' ' ) {
			p++;
		}
		if ( *p ) {
			*p++ = '\0';
		}
	}
	if ( !argc ) {
		return;
	}
	Com_DPrintf( "coop: commande de l'hote '%s' (etat %i)\n", argv[0], dl->state );

	if ( !strcmp( argv[0], "coopdl_again" ) ) {
		// the host got a pack from another joiner after we finished our round:
		// start a fresh one. The whole struct has to go, not just the state -
		// offerCount / listGot / needCount accumulate and are otherwise only
		// cleared when the connection drops.
		if ( dl->state == CLDL_IDLE || dl->state == CLDL_DONE || dl->state == CLDL_FAILED ) {
			memset( dl, 0, sizeof( *dl ) );	// none of those states holds an open file
			dl->state = CLDL_IDLE;
			CL_CoopTransferHello();
		}
		return;
	}

	if ( !strcmp( argv[0], "coopdl_list" ) ) {
		int chunk, total, i;

		if ( dl->state != CLDL_WAITLIST || argc < 3 ) {
			return;
		}
		chunk = atoi( argv[1] );
		total = atoi( argv[2] );
		if ( chunk == 0 || total <= 0 ) {
			// "coopdl_list 0 0": sv_coopTransfer 0 or nothing to offer; the manual ready button stays
			Com_Printf( "coop: l'hote n'a pas de skins a transferer\n" );
			CL_CoopSetState( CLDL_IDLE, "" );
			CL_CoopSetStatus( "" );
			CL_CoopUploadStart();	// coop: we may still have one for it
			return;
		}
		if ( total > MAX_COOP_OFFER ) {
			total = MAX_COOP_OFFER;
		}
		dl->listTotal = total;
		for ( i = 3; i < argc && dl->offerCount < total; i++ ) {
			// <sum>:<size>:<name>
			char *sizeStr = strchr( argv[i], ':' ), *nameStr;
			coopPakInfo_t *info;

			if ( !sizeStr ) {
				continue;
			}
			*sizeStr++ = '\0';
			nameStr = strchr( sizeStr, ':' );
			if ( !nameStr ) {
				continue;
			}
			*nameStr++ = '\0';
			info = &dl->offer[dl->offerCount];
			info->checksum = (int)strtoul( argv[i], NULL, 16 );
			info->size = atoi( sizeStr );
			FS_CoopSanitizeName( nameStr, info->name, sizeof( info->name ) );
			if ( info->size <= 0 || CL_CoopOfferByChecksum( info->checksum ) != NULL ) {
				continue;
			}
			dl->offerCount++;
			dl->listGot++;
		}
		if ( dl->listGot >= dl->listTotal ) {
			CL_CoopTransferListComplete();
		}
		return;
	}

	if ( !Q_strncmp( argv[0], "coopup_", 7 ) ) {
		CL_CoopUploadServerCommand( argc, argv, argc > 1 ? s + ( argv[1] - buf ) : NULL );
		return;
	}

	if ( !strcmp( argv[0], "coopdl_err" ) ) {
		const char *why = ( argc > 1 ) ? s + ( argv[1] - buf ) : "";

		if ( dl->state == CLDL_WAITLIST || dl->state == CLDL_RECEIVING ) {
			Com_Printf( S_COLOR_YELLOW "coop: transfert refuse par l'hote (%s)\n", why );
			CL_CoopCloseCurrent( qtrue );
			CL_CoopSetState( CLDL_FAILED, "error" );
			CL_CoopSetStatus( va( "L'hote a interrompu le transfert des skins (%s).", why ) );
			Cvar_Set( "cl_coopTransferPct", "0" );
		}
		return;
	}
}

/*
==================
CL_CoopTransferComplete

Every requested file is written, verified and loaded.
==================
*/
static void CL_CoopTransferComplete( void ) {
	clientCoopDownload_t *dl = &clc.coopdl;
	const float secs = ( cls.realtime - dl->startTime ) / 1000.0f;

	CL_AddReliableCommand( "coopdl_done" );
	CL_CoopSetState( CLDL_DONE, "done" );
	Cvar_Set( "cl_coopTransferPct", "100" );
	CL_CoopSetStatus( va( "Skins de l'hote recus (%i fichier%s, %.1f Mo).", dl->downloadedFiles, dl->downloadedFiles > 1 ? "s" : "", dl->totalBytes / ( 1024.0f * 1024.0f ) ) );
	Com_Printf( "coop: transfert termine (%i fichier(s), %.1f Mo, %.1f s)\n", dl->downloadedFiles, dl->totalBytes / ( 1024.0f * 1024.0f ), secs );
	if ( dl->downloadedFiles > 0 ) {
		// we were the reason the host waited: mark ourselves ready
		Cvar_Set( "coop_ready", "1" );
	}
	CL_CoopUploadStart();	// coop: now the other way round, our own mods
}

/*
==================
CL_ParseDownload

One svc_download block. Port of codemp's CL_ParseDownload: block 0 carries the
file size and "<sum>:<name>", a zero-length block is the EOF; blocks out of
sequence are dropped (the server resends its window on a duplicate ack).
==================
*/
void CL_ParseDownload( msg_t *msg ) {
	clientCoopDownload_t *dl = &clc.coopdl;
	static byte data[COOP_DL_BLK];
	char header[MAX_STRING_CHARS];
	int block, size = -1, len;

	block = MSG_ReadLong( msg );
	header[0] = '\0';
	if ( block == 0 ) {
		size = MSG_ReadLong( msg );
		Q_strncpyz( header, MSG_ReadString( msg ), sizeof( header ) );
	}
	len = MSG_ReadShort( msg );
	if ( block < 0 || len < 0 || len > COOP_DL_BLK ) {
		Com_Error( ERR_DROP, "CL_ParseDownload: bloc %i de %i octets", block, len );
	}
	MSG_ReadData( msg, data, len );

	if ( dl->state != CLDL_RECEIVING ) {
		return;	// late blocks after an abort
	}
	dl->ackPending = qtrue;

	if ( block == 0 && !dl->cur.file && dl->cur.block == 0 ) {
		// first block of the next file
		char *name = strchr( header, ':' );
		const coopPakInfo_t *info;
		int sum;

		if ( !name ) {
			return;
		}
		*name++ = '\0';
		sum = (int)strtoul( header, NULL, 16 );
		if ( dl->needIndex >= dl->needCount || sum != dl->need[dl->needIndex] ) {
			Com_DPrintf( "coop: bloc 0 de %08x ignore (attendu %08x)\n", sum, dl->needIndex < dl->needCount ? dl->need[dl->needIndex] : 0 );
			return;	// a late retransmit of the previous file
		}
		info = CL_CoopOfferByChecksum( sum );
		if ( !info || size != info->size ) {
			CL_CoopTransferFail( "size", va( "%s: %i octets annonces, %i attendus", name, size, info ? info->size : -1 ) );
			return;
		}
		dl->cur.sum = sum;
		Q_strncpyz( dl->cur.name, info->name, sizeof( dl->cur.name ) );
		dl->cur.size = size;
		dl->cur.count = 0;
		dl->cur.file = FS_CoopOpenDownload( sum, info->name, dl->cur.relTmp, sizeof( dl->cur.relTmp ) );
		if ( !dl->cur.file ) {
			CL_CoopTransferFail( "disk", dl->cur.relTmp );
			return;
		}
		Com_Printf( "coop: telechargement de %s (%.1f Mo)\n", dl->cur.name, size / ( 1024.0f * 1024.0f ) );
	}
	if ( !dl->cur.file ) {
		return;	// blocks of a file whose block 0 we have not seen (yet)
	}
	if ( dl->cur.block != block ) {
		return;	// duplicate or gap: the ack at the end of this message says where we are
	}

	if ( len ) {
		if ( FS_Write( data, len, dl->cur.file ) != len ) {
			CL_CoopTransferFail( "disk", dl->cur.relTmp );
			return;
		}
		dl->cur.count += len;
		dl->doneBytes += len;
	}
	dl->cur.block++;
	dl->lastBlockTime = cls.realtime;

	if ( !len ) {
		// EOF: acknowledge it now under this file's index (the server opens the
		// next file on it), then close, verify, rename, load
		char why[256];

		dl->ackPending = qfalse;
		CL_AddReliableCommand( va( "coopdl_ack %i %i", dl->needIndex, dl->cur.block - 1 ) );
		FS_FCloseFile( dl->cur.file );
		dl->cur.file = 0;
		if ( dl->cur.count != dl->cur.size ) {
			CL_CoopTransferFail( "size", va( "%s: %i octets recus sur %i", dl->cur.name, dl->cur.count, dl->cur.size ) );
			return;
		}
		if ( !FS_CoopFinishDownload( dl->cur.relTmp, dl->cur.sum, dl->cur.name, why, sizeof( why ) ) ) {
			dl->cur.relTmp[0] = '\0';	// FS_CoopFinishDownload removed the file
			CL_CoopTransferFail( Q_stricmpn( why, "checksum", 8 ) ? "content" : "checksum", va( "%s: %s", dl->cur.name, why ) );
			return;
		}
		dl->cur.relTmp[0] = '\0';
		dl->downloadedFiles++;
		dl->needIndex++;
		Com_Printf( "coop: %s termine et charge\n", dl->cur.name );
		// the renderer cached the failed lookups of the host's model (stormtrooper
		// fallback): forget them now, before the cgame rebuilds the players
		// (FS_CoopAddPak bumped cl_coopPaksGen)
		CL_CoopCheckPaksGen();
		dl->cur.block = 0;	// the next file starts at block 0 (acks carry the file index)
		if ( dl->needIndex >= dl->needCount ) {
			CL_CoopTransferComplete();
		}
	}
}

/*
==================
CL_CoopTransferEndOfMessage

End of CL_ParseServerMessage: one cumulative ack per message that carried
blocks, and only while the server is keeping up with our reliable commands.
The queue is MAX_RELIABLE_COMMANDS deep and the server empties it at the rate
it reads our packets; a fast download sends more messages than that, so an ack
per message would fill the queue and CL_AddReliableCommand would drop the
session ("Client command overflow"). Acks are cumulative: skipping one costs
nothing, the next message carries a further block number.
==================
*/
void CL_CoopTransferEndOfMessage( void ) {
	clientCoopDownload_t *dl = &clc.coopdl;

	if ( !dl->ackPending ) {
		return;
	}
	if ( dl->state != CLDL_RECEIVING ) {
		dl->ackPending = qfalse;
		return;
	}
	if ( clc.reliableSequence - clc.reliableAcknowledge >= COOP_DL_ACK_PENDING ) {
		return;		// still ours to send, with a higher block, once the server catches up
	}
	dl->ackPending = qfalse;
	CL_AddReliableCommand( va( "coopdl_ack %i %i", dl->needIndex, dl->cur.block - 1 ) );
}

/*
==================
CL_CoopTransferFrame

Timeouts and the lobby's live status line.
==================
*/
void CL_CoopTransferFrame( void ) {
	clientCoopDownload_t *dl = &clc.coopdl;

	CL_CoopCheckPaksGen();	// a pack was added or removed: the renderer forgets what it missed
	if ( cls.state < CA_CONNECTED ) {
		return;
	}
	CL_CoopUploadFrame();	// coop: our own mods going up to the host
	if ( dl->state == CLDL_WAITLIST ) {
		if ( cls.realtime - dl->helloTime > COOP_DL_LIST_TIMEOUT ) {
			Com_Printf( "coop: pas de reponse de l'hote sur les skins, on continue sans\n" );
			CL_CoopSetState( CLDL_IDLE, "" );
			CL_CoopSetStatus( "" );
		}
		return;
	}
	if ( dl->state != CLDL_RECEIVING ) {
		return;
	}
	if ( cls.realtime - dl->lastBlockTime > COOP_DL_BLOCK_TIMEOUT ) {
		CL_CoopTransferFail( "timeout", "" );
		return;
	}
	// the ready button is hidden while we download, the console is not
	if ( Cvar_VariableIntegerValue( "coop_ready" ) ) {
		Cvar_Set( "coop_ready", "0" );
	}
	if ( cls.realtime - dl->lastTickTime >= 250 ) {
		const float dt = ( cls.realtime - dl->lastTickTime ) / 1000.0f;
		const float inst = ( dl->doneBytes - dl->bytesAtLastTick ) / dt;
		int pct = dl->totalBytes > 0 ? (int)( (float)dl->doneBytes * 100.0f / (float)dl->totalBytes ) : 0;
		int eta;

		dl->speed = dl->speed > 0 ? dl->speed * 0.7f + inst * 0.3f : inst;
		dl->bytesAtLastTick = dl->doneBytes;
		dl->lastTickTime = cls.realtime;
		eta = dl->speed > 1 ? (int)( ( dl->totalBytes - dl->doneBytes ) / dl->speed + 0.5f ) : -1;
		if ( pct > 99 ) {
			pct = 99;
		}
		Cvar_Set( "cl_coopTransferPct", va( "%i", pct ) );
		if ( eta >= 0 ) {
			CL_CoopSetStatus( va( "Telechargement de %s : %i %%  (%.1f Mo/s, %i s)", dl->cur.name[0] ? dl->cur.name : "...", pct, dl->speed / ( 1024.0f * 1024.0f ), eta ) );
		} else {
			CL_CoopSetStatus( va( "Telechargement de %s : %i %%", dl->cur.name[0] ? dl->cur.name : "...", pct ) );
		}
	}
}

/*
==================
CL_CoopTransferAbort

CL_Disconnect, before clc is wiped: drop a partial file, unload the packs we
added (the files stay for a reconnection; they are deleted at quit and startup).
==================
*/
void CL_CoopTransferAbort( void ) {
	if ( clc.coopdl.state == CLDL_RECEIVING ) {
		Com_Printf( "coop: transfert interrompu par la deconnexion\n" );
	}
	CL_CoopCloseCurrent( qtrue );
	CL_CoopUploadAbort();
	clc.coopdl.state = CLDL_IDLE;
	if ( cl_coopTransferState ) {
		Cvar_Set( "cl_coopTransferState", "" );
		Cvar_Set( "cl_coopTransferStatus", "" );
		Cvar_Set( "cl_coopTransferPct", "0" );
	}
	FS_CoopUnloadPaks();
}
