/*
===========================================================================
Copyright (C) 2026, JACoop contributors

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

// g_coop_lobby.cpp -- the co-op lobby and each joiner's own progression.
//
// Lobby: the host loads a small map (t1_inter) with g_coopLobby set. The
// map's scripts, triggers and cinematic NPCs are not spawned, the player
// list is published in CS_COOP_LOBBY for the lobby menu on every machine,
// and the host picks "Continue" (load) or "New game" from there.
//
// Progression: a joiner is not part of the host's savegame (its entity is a
// live connection), so its force powers, weapons, ammo and inventory are kept
// in a table keyed by the coop_guid its client sends in the userinfo. The
// table follows the player across level changes, disconnections and
// reconnections, and is written next to the host's savegame
// (saves/<name>.coop) so "Continue" brings everyone back where they were.
//
// Force parity: powers the story grants the host (jump, push, saber
// styles...) are mirrored to joiners; the eight powers the player allocates
// (heal, mind trick, protect, absorb, grip, lightning, rage, drain) are
// allocated by each joiner on its own screen, within the host's budget.

#include "g_local.h"
#include "g_items.h"
#include "wp_saber.h"

extern void G_AddWeaponModels( gentity_t *ent );
extern cvar_t *g_char_model;

#define COOP_ALLOC_MASK	( ( 1 << FP_HEAL ) | ( 1 << FP_TELEPATHY ) | ( 1 << FP_PROTECT ) | ( 1 << FP_ABSORB ) \
						| ( 1 << FP_GRIP ) | ( 1 << FP_LIGHTNING ) | ( 1 << FP_RAGE ) | ( 1 << FP_DRAIN ) )

#define MAX_COOP_SAVED			16
#define COOP_SIDECAR_MAGIC		0x50434A31	// "1JCP"
#define COOP_SIDECAR_VERSION	1

typedef struct coopSavedPlayer_s {
	char	guid[32];
	char	name[36];
	int		forcePowerLevel[NUM_FORCE_POWERS];
	int		forcePowersKnown;
	int		forcePowerMax;
	int		weapons;
	int		weapon;
	int		ammo[AMMO_MAX];
	int		inventory[INV_MAX];
	int		saberStylesKnown;
	int		saberAnimLevel;
} coopSavedPlayer_t;

typedef struct coopSidecar_s {
	int					magic;
	int					version;
	int					count;
	coopSavedPlayer_t	players[MAX_COOP_SAVED];
} coopSidecar_t;

static void G_CoopStartFrame( void );

static coopSavedPlayer_t	coopSaved[MAX_COOP_SAVED];
static int					coopNumSaved = 0;
static int					coopLastPoints[MAX_CLIENTS];	// force points we last told each joiner about

static qboolean	coopLobby = qfalse;
static int		coopLobbyMenuTime = 0;
static qboolean	coopStarting = qfalse;			// host pressed NOUVELLE PARTIE: joiners are creating their characters
static qboolean	coopStartSent = qfalse;			// the map command went out
static qboolean	coopCharDone[MAX_CLIENTS];		// joiner validated its character since coop_start

/*
==============================================================================
Saved progression
==============================================================================
*/
static const char *G_CoopGuid( const gentity_t *ent )
{
	static char guid[MAX_CLIENTS][32];
	const int	slot = ent->s.number;
	char		userinfo[MAX_INFO_STRING];

	if ( slot <= 0 || slot >= MAX_CLIENTS )
	{
		return "";
	}
	gi.GetUserinfo( slot, userinfo, sizeof( userinfo ) );
	Q_strncpyz( guid[slot], Info_ValueForKey( userinfo, "coop_guid" ), sizeof( guid[slot] ) );
	return guid[slot];
}

static coopSavedPlayer_t *G_CoopFindSaved( const char *guid, qboolean create )
{
	if ( !guid[0] )
	{
		return NULL;
	}
	for ( int i = 0; i < coopNumSaved; i++ )
	{
		if ( !Q_stricmp( coopSaved[i].guid, guid ) )
		{
			return &coopSaved[i];
		}
	}
	if ( !create )
	{
		return NULL;
	}
	if ( coopNumSaved >= MAX_COOP_SAVED )
	{	// table full: recycle the oldest entry
		memmove( &coopSaved[0], &coopSaved[1], sizeof( coopSaved[0] ) * ( MAX_COOP_SAVED - 1 ) );
		coopNumSaved = MAX_COOP_SAVED - 1;
	}
	coopSavedPlayer_t *s = &coopSaved[coopNumSaved++];
	memset( s, 0, sizeof( *s ) );
	Q_strncpyz( s->guid, guid, sizeof( s->guid ) );
	return s;
}

// remember what a joiner carries (called on disconnect, save, level end)
void G_CoopStoreState( gentity_t *ent )
{
	if ( !ent || ent->s.number <= 0 || ent->s.number >= MAX_CLIENTS || !ent->client || ent->client->pers.connected != CON_CONNECTED )
	{
		return;
	}
	if ( coopLobby )
	{	// nothing is earned in the lobby, and a fresh lobby character must not overwrite a real one
		return;
	}
	coopSavedPlayer_t *s = G_CoopFindSaved( G_CoopGuid( ent ), qtrue );
	if ( !s )
	{
		return;
	}
	const playerState_t *ps = &ent->client->ps;
	Q_strncpyz( s->name, ent->client->pers.netname, sizeof( s->name ) );
	memcpy( s->forcePowerLevel, ps->forcePowerLevel, sizeof( s->forcePowerLevel ) );
	s->forcePowersKnown = ps->forcePowersKnown;
	s->forcePowerMax = ps->forcePowerMax;
	s->weapons = ps->stats[STAT_WEAPONS];
	s->weapon = ps->weapon;
	memcpy( s->ammo, ps->ammo, sizeof( s->ammo ) );
	memcpy( s->inventory, ps->inventory, sizeof( s->inventory ) );
	s->saberStylesKnown = ps->saberStylesKnown;
	s->saberAnimLevel = ps->saberAnimLevel;
}

void G_CoopStoreAll( void )
{
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		G_CoopStoreState( &g_entities[i] );
	}
}

static void G_CoopApplySaved( gentity_t *ent, const coopSavedPlayer_t *s )
{
	playerState_t *ps = &ent->client->ps;

	memcpy( ps->forcePowerLevel, s->forcePowerLevel, sizeof( ps->forcePowerLevel ) );
	ps->forcePowersKnown = s->forcePowersKnown;
	ps->forcePowerMax = s->forcePowerMax;
	ps->forcePower = ps->forcePowerMax;
	ps->stats[STAT_WEAPONS] = s->weapons | ( 1 << WP_NONE );
	memcpy( ps->ammo, s->ammo, sizeof( ps->ammo ) );
	memcpy( ps->inventory, s->inventory, sizeof( ps->inventory ) );
	ps->saberStylesKnown = s->saberStylesKnown;
	ps->saberAnimLevel = s->saberAnimLevel;
	if ( s->weapon > WP_NONE && s->weapon < WP_NUM_WEAPONS && ( ps->stats[STAT_WEAPONS] & ( 1 << s->weapon ) ) )
	{
		ps->weapon = s->weapon;
		G_RemoveWeaponModels( ent );
		G_AddWeaponModels( ent );
	}
	ps->inventory[INV_GOODIE_KEY] = 0;
	ps->inventory[INV_SECURITY_KEY] = 0;
}

// a first-time joiner starts with what the story has given the host so far;
// the allocatable powers stay at zero so it can spend the same points itself
static void G_CoopInitFromHost( gentity_t *ent )
{
	const gclient_t *host = g_entities[0].client;
	playerState_t	*ps = &ent->client->ps;

	if ( !host )
	{
		return;
	}
	for ( int fp = 0; fp < NUM_FORCE_POWERS; fp++ )
	{
		if ( COOP_ALLOC_MASK & ( 1 << fp ) )
		{
			continue;
		}
		if ( host->ps.forcePowerLevel[fp] > ps->forcePowerLevel[fp] )
		{
			ps->forcePowerLevel[fp] = host->ps.forcePowerLevel[fp];
			ps->forcePowersKnown |= ( 1 << fp );
		}
	}
	if ( host->ps.forcePowerMax > ps->forcePowerMax )
	{
		ps->forcePowerMax = host->ps.forcePowerMax;
		ps->forcePower = ps->forcePowerMax;
	}
	ps->saberStylesKnown |= host->ps.saberStylesKnown;
	if ( host->ps.stats[STAT_WEAPONS] & ( 1 << WP_SABER ) )
	{
		ps->stats[STAT_WEAPONS] |= ( 1 << WP_SABER );
	}
}

// a joiner entered the world: bring back its progression, or start it off level with the host
void G_CoopOnJoinerBegin( gentity_t *ent )
{
	if ( !ent || ent->s.number <= 0 || ent->s.number >= MAX_CLIENTS || !ent->client )
	{
		return;
	}
	const coopSavedPlayer_t *s = G_CoopFindSaved( G_CoopGuid( ent ), qfalse );
	if ( s )
	{
		G_CoopApplySaved( ent, s );
		gi.Printf( "coop: %s continues with its saved character (weapons %i, weapon %i)\n",
			ent->client->pers.netname, ent->client->ps.stats[STAT_WEAPONS], ent->client->ps.weapon );
	}
	G_CoopInitFromHost( ent );
	gi.Printf( "coop: %s enters with weapons %i (weapon %i), force max %i\n",
		ent->client->pers.netname, ent->client->ps.stats[STAT_WEAPONS], ent->client->ps.weapon, ent->client->ps.forcePowerMax );
	coopLastPoints[ent->s.number] = -1;
	coopCharDone[ent->s.number] = qfalse;
	if ( coopLobby )
	{	// the lobby, or straight to the character screens when the host already pressed NOUVELLE PARTIE
		gi.SendServerCommand( ent->s.number, coopStarting ? "coopmenu coopCharacter" : "coopmenu coopLobby" );
	}
}

/*
==============================================================================
Force parity with the host
==============================================================================
*/
static int G_CoopAllocTotal( const playerState_t *ps )
{
	int total = 0;
	for ( int fp = 0; fp < NUM_FORCE_POWERS; fp++ )
	{
		if ( COOP_ALLOC_MASK & ( 1 << fp ) )
		{
			total += ps->forcePowerLevel[fp];
		}
	}
	return total;
}

// force points a joiner can still spend (host's allocatable total minus its own)
int G_CoopForcePointsAvailable( const gentity_t *ent )
{
	const gclient_t *host = g_entities[0].client;
	if ( !host || !ent || !ent->client || ent->s.number == 0 )
	{
		return 0;
	}
	return G_CoopAllocTotal( &host->ps ) - G_CoopAllocTotal( &ent->client->ps );
}

static void G_CoopSyncForce( gentity_t *ent )
{
	const gclient_t *host = g_entities[0].client;
	playerState_t	*ps = &ent->client->ps;

	if ( !host || ent->health <= 0 )
	{
		return;
	}
	for ( int fp = 0; fp < NUM_FORCE_POWERS; fp++ )
	{
		if ( COOP_ALLOC_MASK & ( 1 << fp ) )
		{
			continue;
		}
		if ( host->ps.forcePowerLevel[fp] > ps->forcePowerLevel[fp] )
		{
			ps->forcePowerLevel[fp] = host->ps.forcePowerLevel[fp];
			ps->forcePowersKnown |= ( 1 << fp );
		}
	}
	if ( host->ps.forcePowerMax > ps->forcePowerMax )
	{
		ps->forcePowerMax = host->ps.forcePowerMax;
	}
	ps->saberStylesKnown |= host->ps.saberStylesKnown;

	// the saber follows the host: the scripts hand it to "player" only
	// (yavin2's tut_start after the weaponless academy), so a joiner that lost
	// it on a level change gets it back here, in hand if it holds nothing
	if ( ( host->ps.stats[STAT_WEAPONS] & ( 1 << WP_SABER ) ) && !( ps->stats[STAT_WEAPONS] & ( 1 << WP_SABER ) ) )
	{
		ps->stats[STAT_WEAPONS] |= ( 1 << WP_SABER );
		WP_SaberInitBladeData( ent );
		if ( ps->weapon == WP_NONE )
		{
			ps->weapon = WP_SABER;
			ps->weaponstate = WEAPON_READY;
			G_RemoveWeaponModels( ent );
			WP_SaberAddG2SaberModels( ent );
			gi.SendServerCommand( ent->s.number, "wp %i", WP_SABER );	// the joiner's cgame selects it too
		}
		gi.Printf( "coop: %s gets the saber back from the host\n", ent->client->pers.netname );
	}

	// the host may have loaded an older save: never keep more points than it has
	int over = -G_CoopForcePointsAvailable( ent );
	for ( int fp = NUM_FORCE_POWERS - 1; over > 0 && fp >= 0; fp-- )
	{
		if ( ( COOP_ALLOC_MASK & ( 1 << fp ) ) && ps->forcePowerLevel[fp] > 0 )
		{
			ps->forcePowerLevel[fp]--;
			if ( !ps->forcePowerLevel[fp] )
			{
				ps->forcePowersKnown &= ~( 1 << fp );
			}
			over--;
		}
	}

	const int points = G_CoopForcePointsAvailable( ent );
	if ( points != coopLastPoints[ent->s.number] )
	{
		coopLastPoints[ent->s.number] = points;
		if ( points > 0 && !coopLobby )
		{
			gi.SendServerCommand( ent->s.number, "cp \"^3%i point%s de Force a repartir\n^7F6 > Points de Force\"", points, points > 1 ? "s" : "" );
		}
	}
}

// "coopforce l0 l1 ... l(NUM_FORCE_POWERS-1)" from a joiner's allocation screen
void G_CoopForceCommand( gentity_t *ent )
{
	const gclient_t *host = g_entities[0].client;

	if ( !host || !ent || !ent->client || ent->s.number == 0 || gi.argc() < 1 + NUM_FORCE_POWERS )
	{
		return;
	}
	int levels[NUM_FORCE_POWERS], total = 0;
	for ( int fp = 0; fp < NUM_FORCE_POWERS; fp++ )
	{
		levels[fp] = Com_Clampi( 0, NUM_FORCE_POWER_LEVELS - 1, atoi( gi.argv( 1 + fp ) ) );
		if ( COOP_ALLOC_MASK & ( 1 << fp ) )
		{
			total += levels[fp];
		}
	}
	if ( total > G_CoopAllocTotal( &host->ps ) )
	{
		gi.SendServerCommand( ent->s.number, "print \"^1Trop de points de Force demandes.\n\"" );
		return;
	}
	playerState_t *ps = &ent->client->ps;
	for ( int fp = 0; fp < NUM_FORCE_POWERS; fp++ )
	{
		if ( !( COOP_ALLOC_MASK & ( 1 << fp ) ) )
		{
			continue;
		}
		ps->forcePowerLevel[fp] = levels[fp];
		if ( levels[fp] )
		{
			ps->forcePowersKnown |= ( 1 << fp );
		}
		else
		{
			ps->forcePowersKnown &= ~( 1 << fp );
		}
	}
	G_CoopStoreState( ent );
	gi.Printf( "coop: %s allocated its force points (%i of %i)\n", ent->client->pers.netname, total, G_CoopAllocTotal( &host->ps ) );
}

/*
==============================================================================
Lobby
==============================================================================
*/
qboolean G_CoopIsLobby( void )
{
	return coopLobby;
}

// called from InitGame once the cvars exist
void G_CoopLobbyInit( void )
{
	coopLobby = (qboolean)( gi.Cvar_VariableIntegerValue( "g_coopLobby" ) != 0 );
	coopLobbyMenuTime = 0;
	coopStarting = coopStartSent = qfalse;
	memset( coopCharDone, 0, sizeof( coopCharDone ) );
	memset( coopLastPoints, -1, sizeof( coopLastPoints ) );

	// "map" (a new campaign, or the lobby itself) starts everyone over; level
	// transitions carry the host's state in the playersave cvar and keep ours
	char carry[MAX_STRING_CHARS];
	gi.Cvar_VariableStringBuffer( "playersave", carry, sizeof( carry ) );
	if ( !carry[0] )
	{
		coopNumSaved = 0;
	}
}

// the host entered the lobby: open its lobby menu once its in-game UI is up
void G_CoopLobbyClientBegin( const gentity_t *ent )
{
	if ( coopLobby && ent && ent->s.number == 0 )
	{
		coopLobbyMenuTime = level.time + 2500;	// once its client is active (a menu opened earlier is swept away)
	}
}

// the lobby map is a quiet room: no scripts, no triggers, no cinematic cast
qboolean G_CoopLobbySkipEntity( const gentity_t *ent )
{
	if ( !coopLobby || !ent->classname )
	{
		return qfalse;
	}
	if ( !Q_stricmp( ent->classname, "target_scriptrunner" )
		|| !Q_stricmp( ent->classname, "target_level_change" )
		|| !Q_stricmp( ent->classname, "trigger_once" )
		|| !Q_stricmp( ent->classname, "trigger_multiple" )
		|| !Q_strncmp( ent->classname, "NPC_", 4 ) )
	{
		return qtrue;
	}
	return qfalse;
}

static void G_CoopUpdateLobbyList( void )
{
	static char	last[MAX_STRING_CHARS];
	char		now[MAX_STRING_CHARS];

	// phase: L lobby, S starting (joiners create their characters), G playing
	Q_strncpyz( now, coopLobby ? ( coopStarting ? "S" : "L" ) : "G", sizeof( now ) );
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		const gentity_t *ent = G_CoopPlayerSlot( i );
		if ( !ent )
		{
			continue;
		}
		int ready = 2;	// host
		char dl[16] = "";	// pk3 transfer state the engine tags in the userinfo: "" | list | 0..99 | ok | err
		if ( i > 0 )
		{
			char userinfo[MAX_INFO_STRING];
			gi.GetUserinfo( i, userinfo, sizeof( userinfo ) );
			Q_strncpyz( dl, Info_ValueForKey( userinfo, "coop_dl" ), sizeof( dl ) );
			// a joiner still fetching the host's skins is never ready, whatever it says
			const qboolean busy = (qboolean)( !Q_stricmp( dl, "list" ) || ( dl[0] >= '0' && dl[0] <= '9' ) );
			ready = ( atoi( Info_ValueForKey( userinfo, "coop_ready" ) ) && !busy ) ? 1 : 0;
			if ( coopStarting )
			{
				ready = coopCharDone[i] ? 4 : 3;	// character validated / being created
			}
		}
		const int endReady = G_CoopEndLevelReadyState( i );
		if ( endReady >= 0 )
		{	// coop: between two missions, PRET replaces the lobby's ready flag (host included)
			ready = endReady ? 6 : 5;
		}
		Q_strcat( now, sizeof( now ), va( "|%s\t%i\t%s\t%s", ent->client->pers.netname, ready, G_CoopPlayerVar( ent, "g_char_model", g_char_model ), dl ) );
	}
	if ( strcmp( now, last ) )
	{
		Q_strncpyz( last, now, sizeof( last ) );
		gi.SetConfigstring( CS_COOP_LOBBY, now );
	}
}

// once per server frame
void G_CoopLobbyFrame( void )
{
	if ( coopLobbyMenuTime && level.time > coopLobbyMenuTime )
	{
		coopLobbyMenuTime = 0;
		gi.SendServerCommand( 0, "coopmenu coopLobby" );	// through the cgame, like the joiners
	}
	G_CoopEndLevelFrame();	// coop: end of a mission (debrief, vote, loadout); it ends itself if everybody left
	if ( G_CoopNumPlayers() < 2 && !coopLobby && !G_CoopEndLevelActive() )
	{
		return;
	}
	G_CoopUpdateLobbyList();
	G_CoopStartFrame();
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = G_CoopPlayerSlot( i );
		if ( ent )
		{
			G_CoopSyncForce( ent );
		}
	}
}

/*
==============================================================================
Starting a new campaign from the lobby

NOUVELLE PARTIE: the host picks the difficulty and its character (its own
screens), then "coop_start" puts the lobby in phase S: every joiner gets the
character screens (server command coopmenu coopCharacter, no difficulty:
that is the host's) and answers "coop_chardone". The campaign starts when
all of them did (or right away with g_coopRequireReady 0, or when the host
forces it with "coop_go"); "coop_cancel" goes back to phase L.
==============================================================================
*/
static void G_CoopStartCampaign( void )
{
	if ( coopStartSent )
	{
		return;
	}
	coopStartSent = qtrue;
	gi.Printf( "coop: starting the campaign\n" );
	gi.SendServerCommand( -1, "print \"^2La partie commence !\n\"" );
	gi.SendConsoleCommand( "set g_coopLobby 0 ; map yavin1\n" );
}

static qboolean G_CoopAllCharsDone( void )
{
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		if ( G_CoopPlayerSlot( i ) && !coopCharDone[i] )
		{
			return qfalse;
		}
	}
	return qtrue;
}

// client commands coop_start / coop_go / coop_cancel (host) and coop_chardone (joiner)
void G_CoopStartCommand( gentity_t *ent, const char *cmd )
{
	if ( !ent || !ent->client || ent->s.number >= MAX_CLIENTS || !coopLobby )
	{
		return;
	}
	const int slot = ent->s.number;
	if ( slot == 0 && !Q_stricmp( cmd, "coop_start" ) )
	{
		coopStarting = qtrue;
		memset( coopCharDone, 0, sizeof( coopCharDone ) );
		for ( int i = 1; i < MAX_CLIENTS; i++ )
		{
			if ( G_CoopPlayerSlot( i ) )
			{
				gi.SendServerCommand( i, "coopmenu coopCharacter" );
			}
		}
		gi.Printf( "coop: NOUVELLE PARTIE, waiting for the joiners' characters\n" );
	}
	else if ( slot == 0 && !Q_stricmp( cmd, "coop_go" ) )
	{
		coopStarting = qtrue;
		G_CoopStartCampaign();
	}
	else if ( slot == 0 && !Q_stricmp( cmd, "coop_cancel" ) )
	{
		coopStarting = qfalse;
		for ( int i = 1; i < MAX_CLIENTS; i++ )
		{
			if ( G_CoopPlayerSlot( i ) )
			{
				gi.SendServerCommand( i, "coopmenu coopLobby" );
			}
		}
	}
	else if ( slot > 0 && !Q_stricmp( cmd, "coop_chardone" ) )
	{
		coopCharDone[slot] = qtrue;
		gi.Printf( "coop: %s validated its character\n", ent->client->pers.netname );
	}
}

// once per frame in phase S: start when everybody is ready
static void G_CoopStartFrame( void )
{
	if ( !coopStarting || coopStartSent )
	{
		return;
	}
	if ( !gi.Cvar_VariableIntegerValue( "g_coopRequireReady" ) || G_CoopAllCharsDone() )
	{
		G_CoopStartCampaign();
	}
}

/*
==============================================================================
Sidecar next to the host's savegame (engine reads/writes the file)
==============================================================================
*/
int G_CoopSaveState( void *buf, int bufSize )
{
	if ( bufSize < (int)sizeof( coopSidecar_t ) )
	{
		return 0;
	}
	G_CoopStoreAll();
	coopSidecar_t *sc = (coopSidecar_t *)buf;
	memset( sc, 0, sizeof( *sc ) );
	sc->magic = COOP_SIDECAR_MAGIC;
	sc->version = COOP_SIDECAR_VERSION;
	sc->count = coopNumSaved;
	memcpy( sc->players, coopSaved, sizeof( coopSaved ) );
	return sizeof( coopSidecar_t );
}

void G_CoopLoadState( const void *buf, int len )
{
	const coopSidecar_t *sc = (const coopSidecar_t *)buf;
	if ( !buf || len != (int)sizeof( coopSidecar_t ) || sc->magic != COOP_SIDECAR_MAGIC || sc->version != COOP_SIDECAR_VERSION )
	{
		return;
	}
	// merge: the file wins for the players it knows; a player it does not know (joined after
	// that save was written, e.g. the level-start autosave) keeps what it has now
	const int count = Com_Clampi( 0, MAX_COOP_SAVED, sc->count );
	for ( int i = 0; i < count; i++ )
	{
		coopSavedPlayer_t *s = G_CoopFindSaved( sc->players[i].guid, qtrue );
		if ( s )
		{
			*s = sc->players[i];
		}
	}
	gi.Printf( "coop: %i saved co-op character(s) loaded (%i known)\n", count, coopNumSaved );
	// joiners already in the world get their character back right away
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = G_CoopPlayerSlot( i );
		if ( ent && ent->health > 0 )
		{
			const coopSavedPlayer_t *s = G_CoopFindSaved( G_CoopGuid( ent ), qfalse );
			if ( s )
			{
				G_CoopApplySaved( ent, s );
			}
		}
	}
}
