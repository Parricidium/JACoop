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
cvar_t	*cl_coopTransferState;		// ROM: "" | list | downloading | done | error
cvar_t	*cl_coopTransferStatus;		// ROM: text for the lobby
cvar_t	*cl_coopTransferPct;		// ROM
cvar_t	*cl_coopPaksGen;			// ROM: bumped by files.cpp at each pack add / unload

/*
==================
CL_CoopTransferInit
==================
*/
void CL_CoopTransferInit( void ) {
	cl_coopTransfer = Cvar_Get( "cl_coopTransfer", "1", CVAR_ARCHIVE );
	cl_coopTransferMaxMB = Cvar_Get( "cl_coopTransferMaxMB", "300", CVAR_ARCHIVE );
	cl_coopTransferState = Cvar_Get( "cl_coopTransferState", "", CVAR_ROM );
	cl_coopTransferStatus = Cvar_Get( "cl_coopTransferStatus", "", CVAR_ROM );
	cl_coopTransferPct = Cvar_Get( "cl_coopTransferPct", "0", CVAR_ROM );
	cl_coopPaksGen = Cvar_Get( "cl_coopPaksGen", "0", CVAR_ROM );
	Cvar_Set( "cl_coopTransferState", "" );
	Cvar_Set( "cl_coopTransferStatus", "" );
	Cvar_Set( "cl_coopTransferPct", "0" );
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
			Com_Printf( "coop: les skins de l'hote depassent cl_coopTransferMaxMB (%i Mo)\n", cl_coopTransferMaxMB->integer );
			CL_CoopTransferFail( "size", dl->offer[i].name );
			return;
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

	block = MSG_ReadShort( msg );
	header[0] = '\0';
	if ( block == 0 ) {
		size = MSG_ReadLong( msg );
		Q_strncpyz( header, MSG_ReadString( msg ), sizeof( header ) );
	}
	len = MSG_ReadShort( msg );
	if ( len < 0 || len > COOP_DL_BLK ) {
		Com_Error( ERR_DROP, "CL_ParseDownload: bloc de %i octets", len );
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
	if ( ( dl->cur.block & 0xffff ) != block ) {
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
		// the renderer cached the failed lookups of the host's model (stormtrooper fallback):
		// forget them now, before the cgame rebuilds the players (FS_CoopAddPak bumped cl_coopPaksGen)
		if ( cls.rendererStarted && re.CoopForgetMissing ) {
			re.CoopForgetMissing();
		}
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
blocks (the server acks our reliable commands only with its snapshots, so one
per block would overflow the 64-command window).
==================
*/
void CL_CoopTransferEndOfMessage( void ) {
	clientCoopDownload_t *dl = &clc.coopdl;

	if ( !dl->ackPending ) {
		return;
	}
	dl->ackPending = qfalse;
	if ( dl->state != CLDL_RECEIVING ) {
		return;
	}
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

	if ( cls.state < CA_CONNECTED ) {
		return;
	}
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
	clc.coopdl.state = CLDL_IDLE;
	if ( cl_coopTransferState ) {
		Cvar_Set( "cl_coopTransferState", "" );
		Cvar_Set( "cl_coopTransferStatus", "" );
		Cvar_Set( "cl_coopTransferPct", "0" );
	}
	FS_CoopUnloadPaks();
}
