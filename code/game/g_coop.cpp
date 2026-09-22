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

// g_coop.cpp -- game-side co-op support: networking character appearance.
//
// The SP cgame renders every character (players and NPCs) out of its server
// gentity: the ghoul2 model, skin, surfaces and sabers are built at spawn by
// G_SetG2PlayerModel / saber parsing on the host and never travel over the
// wire. A serverless remote client has none of that, so the host packs each
// character's appearance into a "model spec" configstring
//
//     model;skin;surfOff;surfOn;saber1;colors1;saber2;colors2;class;r,g,b,a;hand
//
// (';' because JA's three-part skins already use '|'; hand = "L:path" or
// "R:path", a cutscene prop bolted to that hand, see G_CoopRecordHandModel)
// registered in
// CS_COOP_MODELSPECS and referenced from s.modelindex3, which the entity delta
// already networks. The remote cgame rebuilds the same ghoul2 from that spec
// on first sight (CG_CoopEnsureCharacter in cg_coop.cpp).

#include "g_local.h"
#include "wp_saber.h"

extern int G_FindConfigstringIndex( const char *name, int start, int max, qboolean create );

typedef struct coopAppearance_s {
	char	modelName[MAX_QPATH];
	char	customSkin[MAX_QPATH];
	char	surfOff[MAX_QPATH*4];
	char	surfOn[MAX_QPATH*4];
	char	handModel[MAX_QPATH+2];	// "L:path" / "R:path": cutscene prop bolted to a hand
	char	lastSpec[MAX_STRING_CHARS];
} coopAppearance_t;

// Parallel to g_entities, deliberately outside gentity_t / gclient_t so the
// savegame format is untouched.
static coopAppearance_t	coopAppearance[MAX_GENTITIES];

/*
================
G_CoopRecordModel

Called from G_SetG2PlayerModel with the arguments that built the ghoul2, so
the appearance can be re-described later. modelName may be the stormtrooper
fallback if the requested model failed to load.
================
*/
void G_CoopRecordModel( const gentity_t *ent, const char *modelName, const char *customSkin, const char *surfOff, const char *surfOn )
{
	coopAppearance_t *a = &coopAppearance[ent->s.number];

	Q_strncpyz( a->modelName, modelName ? modelName : "", sizeof( a->modelName ) );
	Q_strncpyz( a->customSkin, customSkin ? customSkin : "", sizeof( a->customSkin ) );
	Q_strncpyz( a->surfOff, surfOff ? surfOff : "", sizeof( a->surfOff ) );
	Q_strncpyz( a->surfOn, surfOn ? surfOn : "", sizeof( a->surfOn ) );
	a->lastSpec[0] = '\0';	// force a re-publish on the next frame
}

/*
================
G_CoopRecordSkinPath

SET_SKIN swaps the skin of a built model by full path
("models/players/tavion_new/model_possessed.skin"). The spec names skins
the G_SetG2PlayerModel way, so keep the "possessed" part when the path is
this model's; a skin from another model directory cannot be described.
================
*/
void G_CoopRecordSkinPath( const gentity_t *ent, const char *skinPath )
{
	coopAppearance_t *a = &coopAppearance[ent->s.number];
	char prefix[MAX_QPATH];

	if ( !a->modelName[0] )
	{
		return;
	}
	Com_sprintf( prefix, sizeof( prefix ), "models/players/%s/model_", a->modelName );
	if ( Q_stricmpn( skinPath, prefix, strlen( prefix ) ) )
	{
		if ( g_developer->integer )
		{
			gi.Printf( "coop: ent %i SET_SKIN '%s' is not a skin of '%s', joiners keep theirs\n", ent->s.number, skinPath, a->modelName );
		}
		return;
	}
	Q_strncpyz( a->customSkin, skinPath + strlen( prefix ), sizeof( a->customSkin ) );
	char *dot = strrchr( a->customSkin, '.' );
	if ( dot )
	{
		*dot = '\0';
	}
	if ( !Q_stricmp( a->customSkin, "default" ) )
	{
		a->customSkin[0] = '\0';
	}
	a->lastSpec[0] = '\0';
}

/*
================
G_CoopRecordHandModel

A cutscene prop bolted to a hand (SET_ADDLHANDBOLT_MODEL / RHAND: datapad,
comlink, scepter) is ghoul2 data the joiners never see; publish it as the
spec's last field, "L:path" or "R:path" (stock keeps one such model per
character, ent->cinematicModel). An empty path removes it.
================
*/
void G_CoopRecordHandModel( const gentity_t *ent, char side, const char *modelPath )
{
	coopAppearance_t *a = &coopAppearance[ent->s.number];

	if ( modelPath && modelPath[0] )
	{
		Com_sprintf( a->handModel, sizeof( a->handModel ), "%c:%s", side, modelPath );
	}
	else
	{
		a->handModel[0] = '\0';
	}
	a->lastSpec[0] = '\0';
}

/*
================
G_CoopClearAppearance

Entity slot is being freed or reused: forget its appearance so a later
occupant does not inherit it.
================
*/
/*
================
G_CoopSoundSetIndex

CS_COOP_SOUNDSETS slot of an ambient sound set name (allocated on first
use); 0 when the range is full (the remote client then just lacks that set).
================
*/
int G_CoopSoundSetIndex( const char *name )
{
	char	s[MAX_STRING_CHARS];
	int		i;

	if ( !name || !name[0] )
	{
		return 0;
	}
	for ( i = 1; i < MAX_COOP_SOUNDSETS; i++ )
	{
		gi.GetConfigstring( CS_COOP_SOUNDSETS + i, s, sizeof( s ) );
		if ( !s[0] )
		{
			gi.SetConfigstring( CS_COOP_SOUNDSETS + i, name );
			return i;
		}
		if ( !Q_stricmp( s, name ) )
		{
			return i;
		}
	}
	gi.Printf( "coop: no CS_COOP_SOUNDSETS slot left for '%s'\n", name );
	return 0;
}

void G_CoopClearAppearance( const gentity_t *ent )
{
	memset( &coopAppearance[ent->s.number], 0, sizeof( coopAppearance[0] ) );
}

/*
================
G_CoopModelSpecIndex

CS_COOP_MODELSPECS slot for an appearance spec. Like G_FindConfigstringIndex,
but a full range is not fatal: random tints and saber colours make many
distinct specs over a level, so a slot no live character references any
more is recycled (the remote cgame rebuilds whoever pointed at it when the
configstring changes), and as a last resort the entity keeps its old index.
================
*/
static int G_CoopModelSpecIndex( const gentity_t *ent, const char *spec )
{
	char		s[MAX_STRING_CHARS];
	qboolean	used[MAX_COOP_MODELSPECS];
	int			i;

	for ( i = 1; i < MAX_COOP_MODELSPECS; i++ )
	{
		gi.GetConfigstring( CS_COOP_MODELSPECS + i, s, sizeof( s ) );
		if ( !s[0] )
		{
			gi.SetConfigstring( CS_COOP_MODELSPECS + i, spec );
			return i;
		}
		if ( !Q_stricmp( s, spec ) )
		{
			return i;
		}
	}

	// full: recycle a slot that no character in use still points at
	memset( used, 0, sizeof( used ) );
	for ( i = 0; i < globals.num_entities; i++ )
	{
		const gentity_t *e = &g_entities[i];
		if ( e->inuse && e->client && e != ent && e->s.modelindex3 > 0 && e->s.modelindex3 < MAX_COOP_MODELSPECS )
		{
			used[e->s.modelindex3] = qtrue;
		}
	}
	for ( i = 1; i < MAX_COOP_MODELSPECS; i++ )
	{
		if ( !used[i] )
		{
			if ( g_developer->integer )
			{
				gi.Printf( "coop: appearance specs full, ent %i recycles slot %i\n", ent->s.number, i );
			}
			gi.SetConfigstring( CS_COOP_MODELSPECS + i, spec );
			return i;
		}
	}
	gi.Printf( "coop: appearance specs full, ent %i keeps spec %i (remote clients see its previous look)\n", ent->s.number, ent->s.modelindex3 );
	return ent->s.modelindex3;
}

static void G_CoopAppendSaber( char *spec, int specSize, const saberInfo_t *saber )
{
	if ( !saber || !saber->name || !saber->name[0] )
	{
		Q_strcat( spec, specSize, ";;" );
		return;
	}
	Q_strcat( spec, specSize, va( ";%s;", saber->name ) );
	for ( int b = 0; b < saber->numBlades && b < MAX_BLADES; b++ )
	{
		Q_strcat( spec, specSize, va( "%s%i", b ? "," : "", (int)saber->blade[b].color ) );
	}
}

/*
================
G_CoopUpdateAppearance

Run once per frame for every entity with a client. Rebuilds the spec string
and re-registers it only when something changed (model swap, saber pickup,
colour change), so the per-frame cost is a few string appends.
================
*/
void G_CoopUpdateAppearance( gentity_t *ent )
{
	coopAppearance_t	*a = &coopAppearance[ent->s.number];
	char				spec[MAX_STRING_CHARS];

	if ( !ent->client )
	{
		return;
	}
	if ( ent->s.number > 0 && ent->s.number < MAX_CLIENTS )
	{
		// A joiner's eyePoint/headPoint are only refreshed by the host's cgame
		// while the host can see it; keep them at its eyes from the server
		// side (Force push/pull, LOS and targeting trace from eyePoint).
		renderInfo_t *ri = &ent->client->renderInfo;
		VectorCopy( ent->currentOrigin, ri->eyePoint );
		ri->eyePoint[2] += ent->client->ps.viewheight;
		VectorCopy( ent->client->ps.viewangles, ri->eyeAngles );
		VectorCopy( ri->eyePoint, ri->headPoint );
		VectorCopy( ent->currentOrigin, ri->torsoPoint );
		ri->torsoPoint[2] += ent->client->ps.viewheight * 0.6f;
	}
	// the wire has no health; faces, health bars and corpse handling read it
	ent->s.coopHealth = ent->health;
	ent->s.coopMaxHealth = ( ent->max_health & COOP_MAXHEALTH_MASK )
		| ( ( ent->NPC && ( ent->NPC->scriptFlags & SCF_MORELIGHT ) ) ? COOP_MAXHEALTH_MORELIGHT : 0 );
	// head tracking: interest points are host-only, so only entity targets travel
	ent->s.coopLookTarget = ( ent->client->renderInfo.lookMode == LM_ENT ) ? ent->client->renderInfo.lookTarget : ENTITYNUM_NONE;
	// Force visuals the remote cgame draws from the placeholder's playerState (lightning, drain,
	// grip hand blur, rage/protect/absorb shells, speed trails, push/pull hand refraction)
	{
		const playerState_t *ps = &ent->client->ps;
		int f = ps->forcePowersActive & COOPF_ACTIVE_MASK;
		f |= Q_min( ps->forcePowerLevel[FP_LIGHTNING], 3 ) << COOPF_LVL_LIGHTNING_SHIFT;
		f |= Q_min( ps->forcePowerLevel[FP_DRAIN], 3 ) << COOPF_LVL_DRAIN_SHIFT;
		f |= Q_min( ps->forcePowerLevel[FP_PROTECT], 3 ) << COOPF_LVL_PROTECT_SHIFT;
		f |= Q_min( ps->forcePowerLevel[FP_ABSORB], 3 ) << COOPF_LVL_ABSORB_SHIFT;
		if ( ps->forceDrainEntityNum >= ENTITYNUM_WORLD )
		{
			f |= COOPF_DRAIN_AREA;
		}
		if ( ps->powerups[PW_FORCE_PUSH] > level.time )
		{
			f |= COOPF_PUSH_LHAND;
		}
		if ( ps->powerups[PW_FORCE_PUSH_RHAND] > level.time )
		{
			f |= COOPF_PUSH_RHAND;
		}
		ent->s.coopForce = f;
		// victim electrocution: the wire bit sticks on corpses, the timer decides
		ent->s.coopShockTime = ps->powerups[PW_SHOCKED];
	}
	if ( !a->modelName[0] && ent->ghoul2.size() && ent->playerModel >= 0 && ent->playerModel < ent->ghoul2.size() )
	{
		// Savegame load: the entity came back with its ghoul2 (and its skin as
		// a CS_CHARSKINS index) but this record is not part of the save, so
		// rebuild it from them - otherwise the host and every NPC of the save
		// stay invisible to the joiners.
		const char *file = ent->ghoul2[ent->playerModel].mFileName;
		if ( !Q_stricmpn( file, "models/players/", 15 ) )
		{
			char model[MAX_QPATH], skin[MAX_QPATH * 2];
			Q_strncpyz( model, file + 15, sizeof( model ) );
			char *slash = strchr( model, '/' );
			if ( slash )
			{
				*slash = '\0';
			}
			skin[0] = '\0';
			const int skinIndex = ent->ghoul2[ent->playerModel].mCustomSkin;
			if ( skinIndex > 0 )
			{
				char cs[MAX_QPATH * 2];
				gi.GetConfigstring( CS_CHARSKINS + skinIndex, cs, sizeof( cs ) );
				const char *bar = strchr( cs, '|' );
				const char *base = strrchr( cs, '/' );
				if ( bar )
				{	// "models/players/X/|head|torso|legs"
					Q_strncpyz( skin, bar + 1, sizeof( skin ) );
				}
				else if ( base && !Q_stricmpn( base + 1, "model_", 6 ) )
				{	// "models/players/X/model_<skin>.skin"
					Q_strncpyz( skin, base + 7, sizeof( skin ) );
					char *dot = strrchr( skin, '.' );
					if ( dot )
					{
						*dot = '\0';
					}
					if ( !Q_stricmp( skin, "default" ) )
					{
						skin[0] = '\0';
					}
				}
			}
			G_CoopRecordModel( ent, model, skin[0] ? skin : NULL, NULL, NULL );
			if ( g_developer->integer )
			{
				gi.Printf( "coop: ent %i appearance rebuilt from its ghoul2: '%s' skin '%s'\n", ent->s.number, model, skin );
			}
		}
	}
	if ( !a->modelName[0] )
	{
		// A few NPCs (remote_sp, mouse, seeker) still use the pre-ghoul2 md3
		// legs/torso/head models: no G_SetG2PlayerModel, so nothing recorded.
		// Describe them as "@legs;torso;head" for the remote cgame.
		const renderInfo_t *ri = &ent->client->renderInfo;
		if ( ent->ghoul2.size() || !ri->legsModelName[0] )
		{
			return;
		}
		Com_sprintf( spec, sizeof( spec ), "@%s;%s;%s;", ri->legsModelName, ri->torsoModelName, ri->headModelName );
	}
	else
	{
		Com_sprintf( spec, sizeof( spec ), "%s;%s;%s;%s", a->modelName, a->customSkin, a->surfOff, a->surfOn );
	}
	G_CoopAppendSaber( spec, sizeof( spec ), &ent->client->ps.saber[0] );
	G_CoopAppendSaber( spec, sizeof( spec ), ent->client->ps.dualSabers ? &ent->client->ps.saber[1] : NULL );
	Q_strcat( spec, sizeof( spec ), va( ";%i", (int)ent->client->NPC_class ) );
	const byte *rgba = ent->client->renderInfo.customRGBA;
	Q_strcat( spec, sizeof( spec ), va( ";%i,%i,%i,%i", rgba[0], rgba[1], rgba[2], rgba[3] ) );
	Q_strcat( spec, sizeof( spec ), va( ";%s", a->handModel ) );

	if ( !strcmp( spec, a->lastSpec ) )
	{
		return;
	}
	Q_strncpyz( a->lastSpec, spec, sizeof( a->lastSpec ) );
	ent->s.modelindex3 = G_CoopModelSpecIndex( ent, spec );
}

/*
==============================================================================
Player queries for the AI and triggers

Stock SP hardcodes "the player" as g_entities[0]. With several clients the AI
must consider all of them; these helpers replace those sites.
==============================================================================
*/

/*
================
G_CoopFixClientScriptName

ICARUS resolves script names through a map with one entity per name, and the
mission scripts only know the host as "player". Client slots get fixed names:
slot 0 is "player", the joiners are "player2".."player4". Called wherever an
entity is (re)associated with ICARUS and after a savegame load, so a save
written by an older build (joiner saved as "player") cannot steal the host's
scripts.
================
*/
void G_CoopFixClientScriptName( gentity_t *ent )
{
	if ( !ent || ent->s.number >= MAX_CLIENTS || !ent->client )
	{
		return;
	}
	const char *want = ( ent->s.number == 0 ) ? "player" : va( "player%i", ent->s.number + 1 );
	if ( !ent->script_targetname || Q_stricmp( ent->script_targetname, want ) )
	{
		ent->script_targetname = ( ent->s.number == 0 ) ? (char *)"player" : G_NewString( want );
	}
}

// true for any human player's entity (slots [0, MAX_CLIENTS) are reserved for clients)
qboolean G_CoopIsPlayer( const gentity_t *ent )
{
	return (qboolean)( ent && ent->s.number < MAX_CLIENTS && ent->client && ent->inuse );
}

// number of connected players
int G_CoopNumPlayers( void )
{
	int n = 0;
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( g_entities[i].inuse && g_entities[i].client && g_entities[i].client->pers.connected == CON_CONNECTED )
		{
			n++;
		}
	}
	return n;
}

/*
================
G_CoopNearestPlayer

The closest connected player to org. With aliveOnly, dead players are
skipped. Never returns NULL: falls back to the host player (slot 0) so
callers written for the single-player global keep working.
================
*/
gentity_t *G_CoopNearestPlayer( const vec3_t org, qboolean aliveOnly )
{
	gentity_t	*best = &g_entities[0];
	float		bestDist = -1;

	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( !ent->inuse || !ent->client || ent->client->pers.connected != CON_CONNECTED )
		{
			continue;
		}
		if ( aliveOnly && ( ent->health <= 0 || G_CoopIsDowned( ent ) ) )
		{
			continue;
		}
		const float dist = DistanceSquared( org, ent->currentOrigin );
		if ( bestDist < 0 || dist < bestDist )
		{
			bestDist = dist;
			best = ent;
		}
	}
	return best;
}

// true if at least one connected player is alive
gentity_t *G_CoopPlayerSlot( int i )
{
	gentity_t *ent = &g_entities[i];
	if ( i < 0 || i >= MAX_CLIENTS || !ent->inuse || !ent->client || ent->client->pers.connected != CON_CONNECTED )
	{
		return NULL;
	}
	return ent;
}

qboolean G_CoopAnyPlayerAlive( void )
{
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		const gentity_t *ent = &g_entities[i];
		if ( ent->inuse && ent->client && ent->client->pers.connected == CON_CONNECTED && ent->health > 0 )
		{
			return qtrue;
		}
	}
	return qfalse;
}

// true if org is in the PVS of any connected player (NPC removal / spawn checks)
qboolean G_CoopInAnyPlayerPVS( const vec3_t org )
{
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		const gentity_t *ent = &g_entities[i];
		if ( ent->inuse && ent->client && ent->client->pers.connected == CON_CONNECTED && gi.inPVS( org, ent->currentOrigin ) )
		{
			return qtrue;
		}
	}
	return qfalse;
}

/*
==============================================================================
Death and respawn

Single player ends the mission when the player dies. In co-op a dead player
comes back a few seconds later beside a living teammate, keeping the loadout
they died with; the mission only fails once nobody is left alive.
==============================================================================
*/

#define COOP_RESPAWN_DELAY	4000

qboolean G_CoopIsUp( const gentity_t *ent );	// defined with the downed/revive code below

static int				coopRespawnTime[MAX_CLIENTS];
static playerState_t	coopDeathState[MAX_CLIENTS];	// loadout snapshot taken at death

// true while this player is dead and waiting for a co-op respawn
qboolean G_CoopRespawnPending( const gentity_t *ent )
{
	return (qboolean)( G_CoopIsPlayer( ent ) && coopRespawnTime[ent->s.number] != 0 );
}

gentity_t *G_CoopLivingTeammate( const gentity_t *self )
{
	gentity_t	*best = NULL;
	float		bestDist = -1;

	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( ent == self || !G_CoopIsUp( ent ) )
		{
			continue;
		}
		const float dist = DistanceSquared( self->currentOrigin, ent->currentOrigin );
		if ( bestDist < 0 || dist < bestDist )
		{
			bestDist = dist;
			best = ent;
		}
	}
	return best;
}

/*
================
G_CoopPlayerDied

Called from player_die. Returns qtrue when the death is handled as a co-op
respawn, in which case the caller must not start the mission-failed flow.
================
*/
qboolean G_CoopPlayerDied( gentity_t *self )
{
	if ( !G_CoopIsPlayer( self ) )
	{
		return qfalse;
	}
	G_CoopClearDowned( self );
	if ( !G_CoopLivingTeammate( self ) && !G_CoopDownedActive() )
	{
		return qfalse;	// last one standing in a plain game: the stock mission-failed screen
	}
	// with downed players the respawn waits for someone to be up again (G_CoopDownedFrame
	// reloads the checkpoint otherwise)
	const int delay = ( G_CoopDownedActive() && g_coopRespawnDelay->integer > 0 ) ? g_coopRespawnDelay->integer * 1000 : COOP_RESPAWN_DELAY;
	coopDeathState[self->s.number] = self->client->ps;
	coopRespawnTime[self->s.number] = level.time + delay;
	gi.SendServerCommand( -1, "print \"%s ^3est tombe, retour dans %i s...\n\"", self->client->pers.netname, delay / 1000 );
	return qtrue;
}

extern void G_DisplaceSpawnOrigin( vec3_t origin );
extern qboolean G_SpawnOriginIsFree( vec3_t org );
extern void G_AddWeaponModels( gentity_t *ent );

// move a freshly spawned player next to mate (no-op without one)
void G_CoopPlaceBeside( gentity_t *ent, gentity_t *mate )
{
	if ( !mate || !mate->client || mate == ent )
	{
		return;
	}
	vec3_t origin;
	VectorCopy( mate->currentOrigin, origin );
	origin[2] += 9;
	G_DisplaceSpawnOrigin( origin );
	if ( VectorCompare( origin, mate->currentOrigin ) || Distance( origin, mate->currentOrigin ) < 16.0f )
	{	// the mate did not count as an obstacle (cutscene, non-solid): step aside anyway
		vec3_t right;
		AngleVectors( mate->client->ps.viewangles, NULL, right, NULL );
		VectorMA( mate->currentOrigin, 48.0f, right, origin );
		origin[2] += 9;
		if ( !G_SpawnOriginIsFree( origin ) )
		{
			VectorMA( mate->currentOrigin, -48.0f, right, origin );
			origin[2] += 9;
			if ( !G_SpawnOriginIsFree( origin ) )
			{
				VectorCopy( mate->currentOrigin, origin );
				origin[2] += 9;
			}
		}
	}
	VectorCopy( origin, ent->client->ps.origin );
	VectorCopy( origin, ent->currentOrigin );
	SetClientViewAngle( ent, mate->client->ps.viewangles );
	ent->client->ps.eFlags ^= EF_TELEPORT_BIT;
	gi.linkentity( ent );
}

/*
================
G_CoopGatherJoiners

Teleport joiners beside the host: every one of them when 'all' is set,
otherwise only those the host cannot see (not in its PVS), alive and on foot.
================
*/
void G_CoopGatherJoiners( qboolean all )
{
	gentity_t *host = &g_entities[0];
	if ( !host->inuse || !host->client || host->health <= 0 )
	{
		return;
	}
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( !ent->inuse || !ent->client || ent->client->pers.connected != CON_CONNECTED || ent->health <= 0 )
		{
			continue;
		}
		if ( ent->client->ps.eFlags & EF_LOCKED_TO_WEAPON )
		{
			continue;
		}
		if ( !all && gi.inPVS( host->currentOrigin, ent->currentOrigin ) )
		{
			continue;
		}
		G_CoopPlaceBeside( ent, host );
		gi.Printf( "coop: %s brought to the host\n", ent->client->pers.netname );
	}
}

// "coop_tp": a joiner asks to be brought beside the host (stuck behind a locked
// door...); the host asks to be brought beside its nearest joiner (a script
// locked a door with the joiner on the far side).
void G_CoopTeleportCommand( gentity_t *ent )
{
	if ( !ent || !ent->client || ent->s.number >= MAX_CLIENTS || ent->health <= 0 || G_CoopIsDowned( ent ) )
	{
		return;
	}
	gentity_t *target = NULL;
	if ( ent->s.number == 0 )
	{
		float best = 0;
		for ( int i = 1; i < MAX_CLIENTS; i++ )
		{
			gentity_t *other = &g_entities[i];
			if ( !G_CoopIsUp( other ) )
			{
				continue;
			}
			const float d = DistanceSquared( ent->currentOrigin, other->currentOrigin );
			if ( !target || d < best )
			{
				target = other;
				best = d;
			}
		}
	}
	else
	{
		target = &g_entities[0];
	}
	if ( !target || !target->inuse || !target->client )
	{
		return;
	}
	G_CoopPlaceBeside( ent, target );
	gi.Printf( "coop: %s teleported to %s on request\n", ent->client->pers.netname, target->client->pers.netname );
}

static void G_CoopRespawn( gentity_t *ent )
{
	const int		slot = ent->s.number;
	playerState_t	*dead = &coopDeathState[slot];
	gentity_t		*mate = G_CoopLivingTeammate( ent );

	ClientSpawn( ent, eNO );

	// bring the loadout back
	playerState_t *ps = &ent->client->ps;
	ps->stats[STAT_WEAPONS] = dead->stats[STAT_WEAPONS] | ( 1 << WP_NONE );
	memcpy( ps->ammo, dead->ammo, sizeof( ps->ammo ) );
	memcpy( ps->inventory, dead->inventory, sizeof( ps->inventory ) );
	memcpy( ps->forcePowerLevel, dead->forcePowerLevel, sizeof( ps->forcePowerLevel ) );
	ps->forcePowersKnown = dead->forcePowersKnown;
	ps->forcePowerMax = dead->forcePowerMax;
	ps->forcePower = ps->forcePowerMax;
	ps->saberStylesKnown = dead->saberStylesKnown;
	ps->saberAnimLevel = dead->saberAnimLevel;
	if ( dead->weapon > WP_NONE && dead->weapon < WP_NUM_WEAPONS && ( ps->stats[STAT_WEAPONS] & ( 1 << dead->weapon ) ) )
	{
		ps->weapon = dead->weapon;
		G_RemoveWeaponModels( ent );
		G_AddWeaponModels( ent );
	}

	// come back beside a living teammate rather than at the map start
	G_CoopPlaceBeside( ent, mate );
	ent->health = ps->stats[STAT_HEALTH] = ps->stats[STAT_MAX_HEALTH];

	// a falling death faded this player's screen to black (g_trigger.cpp, g_target.cpp)
	{
		extern void G_CoopFadeClient( const gentity_t *ent, const vec4_t dst, int ms );
		const vec4_t clear = { 0, 0, 0, 0 };
		G_CoopFadeClient( ent, clear, 500 );
	}
}

// run once per server frame
void G_CoopRunRespawns( void )
{
	for ( int slot = 0; slot < MAX_CLIENTS; slot++ )
	{
		if ( !coopRespawnTime[slot] || level.time < coopRespawnTime[slot] )
		{
			continue;
		}
		if ( !G_CoopAnyPlayerUp() )
		{
			continue;	// nobody to come back beside: the all-down flow decides (G_CoopDownedFrame)
		}
		coopRespawnTime[slot] = 0;
		gentity_t *ent = &g_entities[slot];
		if ( ent->inuse && ent->client && ent->client->pers.connected == CON_CONNECTED && ent->health <= 0 )
		{
			G_CoopRespawn( ent );
		}
	}
}

/*
==============================================================================
Cinematic camera replication

Cutscenes run entirely on the host: ICARUS drives the cgame camera
(cg_camera.cpp) through shared memory, which a remote client never sees.
While the host is in a camera, or a scripted screen fade is up outside one
(fade to black before end_level, pre-black before a scene), a broadcast
entity mirrors the host's camera into every snapshot; the remote cgame turns
it back into its own client_camera (cg_coop.cpp):

  s.frame & 1      camera active (else only the fade is meaningful)
  pos/apos.trBase  view origin / angles before the shake (coopCameraOrg/Angles)
  origin2[0]       script FOV, before CG_CalcFOVFromX (coopCameraFovX)
  origin2[1]       cinematic bar height, time2 = bar alpha x 255
  angles2, origin2[2]  fade colour rgb and alpha
==============================================================================
*/

#include "../cgame/cg_local.h"
#include "../cgame/cg_camera.h"

static qboolean coopMissionFailedSent[MAX_CLIENTS];	// per client: told about the mission-failed screen
static gentity_t *coopCameraEnt = NULL;
static qboolean coopCameraWasIn = qfalse;
extern float coopCameraFovX;		// cg_camera.cpp
extern vec3_t coopCameraOrg, coopCameraAngles;

/*
================
G_CoopFadeClient

Screen fades the game code aims at one player (falling deaths: trigger_hurt,
target_kill) go to that player's screen only: the host's own cgame for slot
0, a "fade" server command for a joiner. Both fade from the current colour.
A fade meant for the host alone is not broadcast by G_CoopUpdateCamera,
until the next script fade (CameraFade), which is for everyone.
================
*/
static qboolean coopFadeLocal = qfalse;

void G_CoopFadeClient( const gentity_t *ent, const vec4_t dst, int ms )
{
	if ( !ent || ent->s.number >= MAX_CLIENTS )
	{
		return;
	}
	if ( ent->s.number == 0 )
	{
		vec4_t src, dest;
		VectorCopy4( client_camera.fade_color, src );
		VectorCopy4( dst, dest );
		CGCam_Fade( src, dest, ms );
		coopFadeLocal = qtrue;
		return;
	}
	gi.SendServerCommand( ent->s.number, "fade %g %g %g %g %i", dst[0], dst[1], dst[2], dst[3], ms );
}

// a script fade (CQuake3GameInterface::CameraFade) is for everyone
void G_CoopFadeShared( void )
{
	coopFadeLocal = qfalse;
}

void G_CoopUpdateCamera( void )
{
	// a fade running outside a camera is shared too, unless it is the host's own (G_CoopFadeClient)
	const qboolean fading = (qboolean)( !coopFadeLocal && ( client_camera.fade_color[3] > 0.0f || ( client_camera.info_state & CAMERA_FADING ) != 0 ) );

	if ( !in_camera && !fading )
	{
		if ( coopCameraEnt )
		{
			G_FreeEntity( coopCameraEnt );
			coopCameraEnt = NULL;
		}
		coopCameraWasIn = qfalse;
		return;
	}

	if ( !coopCameraEnt || !coopCameraEnt->inuse || coopCameraEnt->s.eType != ET_COOPCAMERA )
	{
		coopCameraEnt = G_Spawn();
		if ( !coopCameraEnt )
		{
			return;
		}
		coopCameraEnt->classname = "coop_camera";
		coopCameraEnt->s.eType = ET_COOPCAMERA;
		coopCameraEnt->svFlags |= SVF_BROADCAST;	// sent regardless of PVS
		coopCameraEnt->contents = 0;
		coopCameraEnt->clipmask = 0;
	}
	if ( in_camera && !coopCameraWasIn )
	{
		// a cutscene starts: scripts lock doors behind the host and drive the
		// story from where it stands. A joiner left out of sight (a door that
		// closed on it, a fall, a detour) would be stranded, so bring it along.
		G_CoopGatherJoiners( qfalse );
	}
	coopCameraWasIn = (qboolean)in_camera;

	gentity_t *cam = coopCameraEnt;
	cam->s.frame = in_camera ? 1 : 0;
	if ( in_camera )
	{
		VectorCopy( coopCameraOrg, cam->s.origin );
		VectorCopy( coopCameraOrg, cam->s.pos.trBase );
		VectorCopy( coopCameraOrg, cam->currentOrigin );
		VectorCopy( coopCameraAngles, cam->s.angles );
		VectorCopy( coopCameraAngles, cam->s.apos.trBase );
		cam->s.origin2[0] = coopCameraFovX;
	}
	cam->s.pos.trType = cam->s.apos.trType = TR_INTERPOLATE;
	cam->s.origin2[1] = client_camera.bar_height;
	cam->s.time2 = (int)( client_camera.bar_alpha * 255.0f );
	cam->s.origin2[2] = client_camera.fade_color[3];
	VectorCopy( client_camera.fade_color, cam->s.angles2 );
	gi.linkentity( cam );
}

/*
================
G_CoopShake

Camera shakes are host-cgame state (CGCam_Shake). Shake the host as before
and broadcast the same shake as a temp entity for the joiners
(EV_COOP_SHAKE): angles2[0] = intensity (per unit of distance when a range
is given), angles2[1] = range (0 = everyone, else each joiner scales by its
own distance to the origin, as the AI does with "the player"),
time2 = duration, otherEntityNum = one client or ENTITYNUM_NONE.
================
*/
static void G_CoopShakeEvent( const vec3_t origin, float intensity, float range, int duration, int clientNum )
{
	if ( G_CoopNumPlayers() < 2 )
	{
		return;
	}
	gentity_t *te = G_TempEntity( origin, EV_COOP_SHAKE );
	te->svFlags |= SVF_BROADCAST;
	te->s.angles2[0] = intensity;
	te->s.angles2[1] = range;
	te->s.time2 = duration;
	te->s.otherEntityNum = clientNum;
}

// every player's screen (scripts: CAMERA SHAKE)
void G_CoopShakeAll( float intensity, int duration )
{
	CGCam_Shake( intensity, duration );
	G_CoopShakeEvent( g_entities[0].currentOrigin, intensity, 0.0f, duration, ENTITYNUM_NONE );
}

// one player's screen
void G_CoopShakeClient( const gentity_t *ent, float intensity, int duration )
{
	if ( !ent || ent->s.number >= MAX_CLIENTS )
	{
		return;
	}
	if ( ent->s.number == 0 )
	{
		CGCam_Shake( intensity, duration );
		return;
	}
	G_CoopShakeEvent( ent->currentOrigin, intensity, 0.0f, duration, ent->s.number );
}

// the AI's "shake the player by its distance": perUnit x distance for every player within range
void G_CoopShakeNear( const vec3_t origin, float perUnit, float range, int duration )
{
	const gentity_t *host = &g_entities[0];
	if ( host->inuse && host->client )
	{
		const float dist = Distance( host->currentOrigin, origin );
		if ( dist < range )
		{
			CGCam_Shake( perUnit * dist, duration );
		}
	}
	G_CoopShakeEvent( origin, perUnit, range, duration, ENTITYNUM_NONE );
}

/*
================
G_CoopMirrorCvars

The cinematic skip (g_active.cpp G_StartCinematicSkip, CGCam_Disable) and
the script timescale (Q3_SetTimeScale) are host cvars; the joiners learn
them as "skip N" / "ts V" server commands when they change.
================
*/
static int		coopSentSkip = 0;
static float	coopSentTimescale = 1.0f;

static void G_CoopSendJoiners( const char *cmd )
{
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		const gentity_t *cl = &g_entities[i];
		if ( cl->inuse && cl->client && cl->client->pers.connected == CON_CONNECTED )
		{
			gi.SendServerCommand( i, "%s", cmd );
		}
	}
}

void G_CoopMirrorCvars( void )
{
	extern cvar_t *g_skippingcin;
	extern cvar_t *g_timescale;

	if ( g_skippingcin->integer != coopSentSkip )
	{
		coopSentSkip = g_skippingcin->integer;
		G_CoopSendJoiners( va( "skip %i", coopSentSkip ) );
	}
	if ( g_timescale->value != coopSentTimescale )
	{
		coopSentTimescale = g_timescale->value;
		G_CoopSendJoiners( va( "ts %g", coopSentTimescale ) );
	}
}

// forget the camera entity when the level goes away
void G_CoopResetCamera( void )
{
	coopSentSkip = 0;
	coopSentTimescale = 1.0f;
	coopCameraEnt = NULL;
	coopCameraWasIn = qfalse;
	coopFadeLocal = qfalse;
	memset( coopMissionFailedSent, 0, sizeof( coopMissionFailedSent ) );
	memset( coopRespawnTime, 0, sizeof( coopRespawnTime ) );
	G_CoopResetDowned();
}

/*
================
G_CoopUpdateMissionFailed

The mission-failed screen is raised straight in the host's cgame (a script
via Q3_SetMissionFailed, or the last player dying: cg.missionStatusShow /
cg.missionStatusDeadTime). Tell the remote clients once, with the status
text, so they show the same screen.
================
*/
void G_CoopUpdateMissionFailed( void )
{
	extern int statusTextIndex;

	const qboolean allDead = (qboolean)( !G_CoopAnyPlayerAlive() && cg.missionStatusDeadTime && cg.missionStatusDeadTime < level.time );
	if ( !cg.missionStatusShow && !allDead )
	{
		return;
	}
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		const gentity_t *ent = &g_entities[i];
		if ( !coopMissionFailedSent[i] && ent->inuse && ent->client && ent->client->pers.connected == CON_CONNECTED )
		{
			coopMissionFailedSent[i] = qtrue;
			gi.SendServerCommand( i, "mf %i", statusTextIndex );
			gi.SendServerCommand( i, "cad -1" );
			gi.SendServerCommand( i, "coopmenu coopAllDownClient" );
		}
	}
}

/*
================
G_CoopClientBegin

A client (re)entered the world: forget what we told the previous occupant.
================
*/
void G_CoopClientBegin( const gentity_t *ent )
{
	G_CoopLobbyClientBegin( ent );
	if ( ent->s.number >= 0 && ent->s.number < MAX_CLIENTS )
	{
		coopMissionFailedSent[ent->s.number] = qfalse;
	}
	if ( ent->s.number > 0 && ent->s.number < MAX_CLIENTS )
	{	// what G_CoopMirrorCvars already told the others
		if ( coopSentSkip )
		{
			gi.SendServerCommand( ent->s.number, "skip %i", coopSentSkip );
		}
		if ( coopSentTimescale != 1.0f )
		{
			gi.SendServerCommand( ent->s.number, "ts %g", coopSentTimescale );
		}
	}
}

/*
==============================================================================
Downed players and revives (PUBG style)

With g_coopDowned and at least two players, a killing blow no longer kills:
the player drops to the ground (health 1, knockdown animation held, immune,
no input but the view) and bleeds out for g_coopBleedOut seconds. A standing
teammate within g_coopReviveRange holding +coop_revive for g_coopReviveTime
seconds brings it back up with part of its health. Bleeding out is a real
death: the existing respawn brings the player back beside someone still up.
When nobody is up any more the host is offered the last checkpoint
(load *respawn) and, after g_coopAllDownAuto seconds, it is reloaded anyway.

The state lives here on the host; the player itself reads it from its
playerState stats (STAT_COOP_DOWN / STAT_COOP_REVIVE / STAT_COOP_REVIVER) and
everyone sees a downed teammate through the PW_COOP_DOWNED powerup bit of
its entityState (cg_coop.cpp draws the markers and the bars).
==============================================================================
*/

extern void NPC_SetAnim( gentity_t *ent, int setAnimParts, int anim, int setAnimFlags, int iBlend = SETANIM_BLEND_DEFAULT );
extern qboolean PM_InKnockDownNoGetup( playerState_t *ps );
extern void WP_ForcePowerStop( gentity_t *self, forcePowers_t forcePower );
extern void NPC_SetPainEvent( gentity_t *self );
extern void G_ClearEnemy( gentity_t *self );
extern bool in_camera;

typedef struct coopDown_s {
	int		downTime;			// level.time the player went down, 0 = up
	int		bleedOutTime;		// level.time it dies for real
	int		reviverNum;			// who is reviving it, else ENTITYNUM_NONE
	int		reviveStartTime;
	int		revivingNum;		// (standing players) who I am reviving, else ENTITYNUM_NONE
} coopDown_t;

static coopDown_t	coopDown[MAX_CLIENTS];
static int			coopAllDownTime;		// level.time nobody was up any more, 0 = someone is
static int			coopAllDownLastSec = -1;
static int			coopBleedingOutNum = -1;	// the player G_CoopDownedFrame is killing for real right now

cvar_t	*g_coopDowned;
cvar_t	*g_coopBleedOut;
cvar_t	*g_coopReviveTime;
cvar_t	*g_coopReviveRange;
cvar_t	*g_coopReviveHealth;
cvar_t	*g_coopRespawnDelay;
cvar_t	*g_coopAllDownAuto;

void G_CoopInitDownedCvars( void )
{
	g_coopDowned = gi.cvar( "g_coopDowned", "1", CVAR_ARCHIVE );
	g_coopBleedOut = gi.cvar( "g_coopBleedOut", "60", CVAR_ARCHIVE );
	g_coopReviveTime = gi.cvar( "g_coopReviveTime", "3", CVAR_ARCHIVE );
	g_coopReviveRange = gi.cvar( "g_coopReviveRange", "80", CVAR_ARCHIVE );
	g_coopReviveHealth = gi.cvar( "g_coopReviveHealth", "40", CVAR_ARCHIVE );
	g_coopRespawnDelay = gi.cvar( "g_coopRespawnDelay", "10", CVAR_ARCHIVE );
	g_coopAllDownAuto = gi.cvar( "g_coopAllDownAuto", "20", CVAR_ARCHIVE );
}

// the level goes away (or comes back): nobody is down
void G_CoopResetDowned( void )
{
	memset( coopDown, 0, sizeof( coopDown ) );
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		coopDown[i].reviverNum = coopDown[i].revivingNum = ENTITYNUM_NONE;
	}
	coopAllDownTime = 0;
	coopAllDownLastSec = -1;
	coopBleedingOutNum = -1;
}

// downed players exist only in a real co-op game
qboolean G_CoopDownedActive( void )
{
	return (qboolean)( g_coopDowned && g_coopDowned->integer && !G_CoopIsLobby() && G_CoopNumPlayers() >= 2 );
}

qboolean G_CoopIsDowned( const gentity_t *ent )
{
	return (qboolean)( G_CoopIsPlayer( ent ) && coopDown[ent->s.number].downTime != 0 );
}

// connected, alive and not down
qboolean G_CoopIsUp( const gentity_t *ent )
{
	return (qboolean)( G_CoopIsPlayer( ent ) && ent->client->pers.connected == CON_CONNECTED && ent->health > 0 && !G_CoopIsDowned( ent ) );
}

qboolean G_CoopAnyPlayerUp( void )
{
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( G_CoopIsUp( &g_entities[i] ) )
		{
			return qtrue;
		}
	}
	return qfalse;
}

static void G_CoopSetReviveStats( gentity_t *ent, int progress, int other )
{
	if ( ent && ent->client )
	{
		ent->client->ps.stats[STAT_COOP_REVIVE] = progress;
		ent->client->ps.stats[STAT_COOP_REVIVER] = other;
	}
}

// the reviver stops (key released, moved, hit, target gone)
void G_CoopReviveCancel( gentity_t *reviver )
{
	if ( !G_CoopIsPlayer( reviver ) )
	{
		return;
	}
	coopDown_t *me = &coopDown[reviver->s.number];
	if ( me->revivingNum >= 0 && me->revivingNum < MAX_CLIENTS )
	{
		coopDown_t *target = &coopDown[me->revivingNum];
		if ( target->reviverNum == reviver->s.number )
		{
			target->reviverNum = ENTITYNUM_NONE;
			target->reviveStartTime = 0;
			G_CoopSetReviveStats( &g_entities[me->revivingNum], 0, ENTITYNUM_NONE );
		}
		if ( reviver->client->ps.legsAnim == BOTH_FORCEHEAL_START )
		{
			reviver->client->ps.legsAnimTimer = reviver->client->ps.torsoAnimTimer = 0;
			NPC_SetAnim( reviver, SETANIM_BOTH, BOTH_FORCEHEAL_STOP, SETANIM_FLAG_OVERRIDE|SETANIM_FLAG_HOLD );
		}
	}
	me->revivingNum = ENTITYNUM_NONE;
	G_CoopSetReviveStats( reviver, 0, ENTITYNUM_NONE );
}

// back on its feet (revived), or dead for real, or respawned
void G_CoopClearDowned( gentity_t *ent )
{
	if ( !G_CoopIsPlayer( ent ) )
	{
		return;
	}
	coopDown_t *d = &coopDown[ent->s.number];
	if ( d->reviverNum >= 0 && d->reviverNum < MAX_CLIENTS )
	{
		G_CoopReviveCancel( &g_entities[d->reviverNum] );
	}
	if ( d->revivingNum != ENTITYNUM_NONE )
	{
		G_CoopReviveCancel( ent );
	}
	d->downTime = d->bleedOutTime = d->reviveStartTime = 0;
	d->reviverNum = d->revivingNum = ENTITYNUM_NONE;
	ent->client->ps.powerups[PW_COOP_DOWNED] = 0;
	ent->client->ps.stats[STAT_COOP_DOWN] = 0;
	G_CoopSetReviveStats( ent, 0, ENTITYNUM_NONE );
}

/*
================
G_CoopTryDown

Called by G_Damage just before a player would die. Returns qtrue when the
death was turned into a downed state instead.
================
*/
qboolean G_CoopTryDown( gentity_t *targ, gentity_t *attacker, int mod, int dflags )
{
	if ( !G_CoopIsPlayer( targ ) || !G_CoopDownedActive() || G_CoopIsDowned( targ ) || targ->s.number == coopBleedingOutNum )
	{
		return qfalse;
	}
	if ( mod == MOD_SUICIDE || mod == MOD_SNIPER || mod == MOD_CRUSH || mod == MOD_TRIGGER_HURT
		|| targ->s.m_iVehicleNum != 0
		|| ( targ->client->ps.eFlags & ( EF_HELD_BY_RANCOR|EF_HELD_BY_WAMPA|EF_HELD_BY_SAND_CREATURE ) )
		|| in_camera
		|| ( mod == MOD_FALLING && targ->client->ps.groundEntityNum == ENTITYNUM_NONE ) )
	{	// no way to lie on the ground there: a real death
		return qfalse;
	}

	playerState_t	*ps = &targ->client->ps;
	coopDown_t		*d = &coopDown[targ->s.number];
	const int		bleed = ( g_coopBleedOut->integer > 0 ? g_coopBleedOut->integer : 60 ) * 1000;

	targ->health = 1;
	ps->stats[STAT_HEALTH] = 1;
	ps->stats[STAT_ARMOR] = 0;
	d->downTime = level.time;
	d->bleedOutTime = level.time + bleed;
	d->reviverNum = ENTITYNUM_NONE;
	d->reviveStartTime = 0;
	if ( d->revivingNum != ENTITYNUM_NONE )
	{
		G_CoopReviveCancel( targ );
	}
	ps->powerups[PW_COOP_DOWNED] = Q3_INFINITE;
	ps->stats[STAT_COOP_DOWN] = bleed;

	// saber off, powers off, no attack
	if ( ps->SaberActive() )
	{
		ps->SaberDeactivate();
		G_SoundIndexOnEnt( targ, CHAN_AUTO, ps->saber[0].soundOff );
	}
	for ( int fp = 0; fp < NUM_FORCE_POWERS; fp++ )
	{
		if ( ps->forcePowersActive & ( 1 << fp ) )
		{
			WP_ForcePowerStop( targ, (forcePowers_t)fp );
		}
	}
	ps->saberLockTime = 0;
	ps->weaponTime = 500;
	G_CoopSetZoomMode( targ, 0 );	// per player

	// fall like a knockdown, from the side the blow came from
	int anim = BOTH_KNOCKDOWN1;
	if ( attacker && attacker != targ )
	{
		vec3_t fwd, dir, angles = { 0, ps->viewangles[YAW], 0 };
		AngleVectors( angles, fwd, NULL, NULL );
		VectorSubtract( targ->currentOrigin, attacker->currentOrigin, dir );
		VectorNormalize( dir );
		if ( DotProduct( fwd, dir ) > 0.2f )
		{
			anim = BOTH_KNOCKDOWN3;	// hit from behind: falls forward
		}
	}
	NPC_SetAnim( targ, SETANIM_BOTH, anim, SETANIM_FLAG_OVERRIDE|SETANIM_FLAG_HOLD );
	ps->legsAnimTimer = ps->torsoAnimTimer = 3000;
	NPC_SetPainEvent( targ );

	// the enemies turn to someone still standing
	for ( int i = MAX_CLIENTS; i < globals.num_entities; i++ )
	{
		gentity_t *e = &g_entities[i];
		if ( e->inuse && e->enemy == targ )
		{
			G_ClearEnemy( e );
		}
	}

	gi.SendServerCommand( -1, "print \"^1%s ^7est a terre !\n\"", targ->client->pers.netname );
	gi.Printf( "coop: %s downed (mod %i), bleeds out in %i s\n", targ->client->pers.netname, mod, bleed / 1000 );
	return qtrue;
}

/*
================
G_CoopRevive

The revive completed: target stands up with part of its health and a short
grace period, reviver leaves the kneeling pose.
================
*/
static void G_CoopRevive( gentity_t *target, gentity_t *reviver )
{
	playerState_t	*ps = &target->client->ps;
	const int		pct = Com_Clampi( 1, 100, g_coopReviveHealth->integer );
	int				health = ps->stats[STAT_MAX_HEALTH] * pct / 100;

	G_CoopClearDowned( target );
	if ( health < 1 )
	{
		health = 1;
	}
	target->health = ps->stats[STAT_HEALTH] = health;
	ps->powerups[PW_INVINCIBLE] = level.time + 2000;
	ps->legsAnimTimer = ps->torsoAnimTimer = 0;
	NPC_SetAnim( target, SETANIM_BOTH, ( ps->forcePowerLevel[FP_LEVITATION] > 0 ) ? Q_irand( BOTH_FORCE_GETUP_F1, BOTH_FORCE_GETUP_F2 ) : BOTH_GETUP1, SETANIM_FLAG_OVERRIDE|SETANIM_FLAG_HOLD );
	ps->weaponTime = 500;
	G_Sound( target, G_SoundIndex( "sound/weapons/force/heal.mp3" ) );
	if ( reviver && reviver->client )
	{
		reviver->client->ps.legsAnimTimer = reviver->client->ps.torsoAnimTimer = 0;
		NPC_SetAnim( reviver, SETANIM_BOTH, BOTH_FORCEHEAL_STOP, SETANIM_FLAG_OVERRIDE|SETANIM_FLAG_HOLD );
		gi.SendServerCommand( -1, "print \"^2%s ^7a releve ^2%s\n\"", reviver->client->pers.netname, target->client->pers.netname );
		gi.Printf( "coop: %s revived by %s (%i hp)\n", target->client->pers.netname, reviver->client->pers.netname, health );
	}
}

/*
================
G_CoopDownedThink

Every ClientThink of a co-op player: a downed one keeps only its view and
its knockdown pose; a standing one holding the revive key beside a downed
teammate revives it.
================
*/
void G_CoopDownedThink( gentity_t *ent, usercmd_t *ucmd )
{
	if ( !G_CoopIsPlayer( ent ) || !ucmd )
	{
		return;
	}
	playerState_t	*ps = &ent->client->ps;
	coopDown_t		*me = &coopDown[ent->s.number];

	if ( me->downTime )
	{	// on the ground: no moves, no buttons, still lying down
		ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0;
		ucmd->buttons = 0;
		ucmd->generic_cmd = 0;
		if ( ent->health > 0 )
		{
			ent->health = 1;
		}
		ps->stats[STAT_HEALTH] = ent->health;
		ps->stats[STAT_ARMOR] = 0;
		ps->weaponTime = 500;
		if ( !PM_InKnockDownNoGetup( ps ) )
		{
			NPC_SetAnim( ent, SETANIM_BOTH, BOTH_KNOCKDOWN1, SETANIM_FLAG_OVERRIDE|SETANIM_FLAG_HOLD );
		}
		if ( ps->legsAnimTimer < 3000 )
		{
			ps->legsAnimTimer = 3000;
		}
		if ( ps->torsoAnimTimer < 3000 )
		{
			ps->torsoAnimTimer = 3000;
		}
		return;
	}

	if ( ent->health <= 0 )
	{
		return;
	}

	// standing: reviving someone?
	const qboolean	key = (qboolean)( ( ucmd->buttons & BUTTON_COOP_REVIVE ) != 0 );
	const qboolean	moving = (qboolean)( ucmd->forwardmove || ucmd->rightmove || ucmd->upmove );
	const float		range = g_coopReviveRange->value > 0 ? g_coopReviveRange->value : 80.0f;

	if ( me->revivingNum != ENTITYNUM_NONE )
	{
		gentity_t	*target = &g_entities[me->revivingNum];
		coopDown_t	*t = &coopDown[me->revivingNum];
		if ( !key || moving || !G_CoopIsDowned( target ) || t->reviverNum != ent->s.number
			|| Distance( ent->currentOrigin, target->currentOrigin ) > range * 1.25f )
		{
			G_CoopReviveCancel( ent );
			return;
		}
		const int	total = ( g_coopReviveTime->value > 0 ? g_coopReviveTime->value : 3.0f ) * 1000;
		int			progress = ( level.time - t->reviveStartTime ) * 100 / total;
		if ( progress >= 100 )
		{
			me->revivingNum = ENTITYNUM_NONE;
			G_CoopSetReviveStats( ent, 0, ENTITYNUM_NONE );
			G_CoopRevive( target, ent );
			return;
		}
		// hold the kneeling pose, no moves, no shots
		ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0;
		ucmd->buttons &= ~( BUTTON_ATTACK|BUTTON_ALT_ATTACK|BUTTON_USE_FORCE|BUTTON_FORCE_LIGHTNING|BUTTON_FORCE_DRAIN|BUTTON_FORCEGRIP );
		ps->weaponTime = 200;
		if ( ps->legsAnim != BOTH_FORCEHEAL_START )
		{
			NPC_SetAnim( ent, SETANIM_BOTH, BOTH_FORCEHEAL_START, SETANIM_FLAG_OVERRIDE|SETANIM_FLAG_HOLD );
		}
		if ( ps->legsAnimTimer < 500 )
		{
			ps->legsAnimTimer = 500;
		}
		if ( ps->torsoAnimTimer < 500 )
		{
			ps->torsoAnimTimer = 500;
		}
		G_CoopSetReviveStats( ent, progress > 0 ? progress : 1, target->s.number );
		G_CoopSetReviveStats( target, progress > 0 ? progress : 1, ent->s.number );
		return;
	}

	if ( !key || moving )
	{
		return;
	}
	// the nearest downed teammate in reach that nobody else is reviving
	gentity_t	*best = NULL;
	float		bestDist = 0;
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t *other = &g_entities[i];
		if ( other == ent || !G_CoopIsDowned( other ) || coopDown[i].reviverNum != ENTITYNUM_NONE )
		{
			continue;
		}
		const float dist = Distance( ent->currentOrigin, other->currentOrigin );
		if ( dist <= range && ( !best || dist < bestDist ) )
		{
			best = other;
			bestDist = dist;
		}
	}
	if ( best )
	{
		me->revivingNum = best->s.number;
		coopDown[best->s.number].reviverNum = ent->s.number;
		coopDown[best->s.number].reviveStartTime = level.time;
		NPC_SetAnim( ent, SETANIM_BOTH, BOTH_FORCEHEAL_START, SETANIM_FLAG_OVERRIDE|SETANIM_FLAG_HOLD );
		ucmd->forwardmove = ucmd->rightmove = ucmd->upmove = 0;
		G_CoopSetReviveStats( ent, 1, best->s.number );
		G_CoopSetReviveStats( best, 1, ent->s.number );
	}
}

/*
================
G_CoopDownedFrame

Once per server frame: bleed-out clocks (paused in cutscenes), the HUD
stats, and the "everyone is down" flow.
================
*/
void G_CoopDownedFrame( void )
{
	const int frame = level.time - level.previousTime;

	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t	*ent = &g_entities[i];
		coopDown_t	*d = &coopDown[i];
		if ( !d->downTime )
		{
			continue;
		}
		if ( !G_CoopIsPlayer( ent ) || ent->client->pers.connected != CON_CONNECTED || ent->health <= 0 )
		{	// gone, or died of something the immunity does not cover
			G_CoopClearDowned( ent );
			continue;
		}
		if ( in_camera )
		{	// the story is talking: nobody bleeds meanwhile
			d->bleedOutTime += frame;
		}
		ent->client->ps.stats[STAT_COOP_DOWN] = ( d->bleedOutTime > level.time ) ? d->bleedOutTime - level.time : 1;
		if ( d->reviverNum == ENTITYNUM_NONE )
		{
			G_CoopSetReviveStats( ent, 0, ENTITYNUM_NONE );
		}
		if ( level.time >= d->bleedOutTime && !in_camera )
		{	// bled out: a real death now (respawn beside someone up, or the all-down flow)
			gi.Printf( "coop: %s bled out\n", ent->client->pers.netname );
			G_CoopClearDowned( ent );
			coopBleedingOutNum = i;
			G_Damage( ent, &g_entities[ENTITYNUM_WORLD], &g_entities[ENTITYNUM_WORLD], NULL, ent->currentOrigin, 1000, DAMAGE_NO_PROTECTION|DAMAGE_NO_ARMOR|DAMAGE_NO_KNOCKBACK, MOD_UNKNOWN );
			coopBleedingOutNum = -1;
		}
	}

	// everyone down (or dead): offer the last checkpoint, reload it after a while
	if ( !G_CoopDownedActive() || in_camera || coopAllDownTime < 0 )
	{
		return;	// (< 0: the reload is on its way)
	}
	if ( G_CoopAnyPlayerUp() )
	{
		if ( coopAllDownTime )
		{	// someone came back (a scripted respawn): drop the screens
			coopAllDownTime = 0;
			gi.SendServerCommand( -1, "coopmenu closeall" );
		}
		return;
	}
	if ( !coopAllDownTime )
	{
		coopAllDownTime = level.time;
		coopAllDownLastSec = -1;
		gi.SendServerCommand( -1, "print \"^1Tous les joueurs sont a terre.\n\"" );
		gi.Printf( "coop: everyone is down\n" );
		for ( int i = 0; i < MAX_CLIENTS; i++ )
		{
			if ( G_CoopPlayerSlot( i ) )
			{
				gi.SendServerCommand( i, "coopmenu %s", i == 0 ? "coopAllDownHost" : "coopAllDownClient" );
			}
		}
	}
	const int auto_ = g_coopAllDownAuto->integer;
	const int left = auto_ > 0 ? auto_ - ( level.time - coopAllDownTime ) / 1000 : -1;
	if ( left != coopAllDownLastSec )
	{
		coopAllDownLastSec = left;
		gi.SendServerCommand( -1, "cad %i", left );
	}
	if ( auto_ > 0 && left <= 0 )
	{
		G_CoopReloadCheckpoint();
	}
}

// the host reloads the last checkpoint (all-down screen, or its button)
void G_CoopReloadCheckpoint( void )
{
	if ( coopAllDownTime < 0 )
	{
		return;	// already asked
	}
	coopAllDownTime = -1;
	gi.SendServerCommand( -1, "print \"^3Retour au dernier point de controle...\n\"" );
	gi.Printf( "coop: reloading the last checkpoint\n" );
	gi.SendConsoleCommand( "load *respawn\n" );
}

/*
==============================================================================
Per-player character settings

Single player builds the player from the host's cvars (g_char_*, g_saber*).
A joiner sends the same cvars in its userinfo (they are CVAR_USERINFO in
G_InitCvars), and the spawn code reads them through here so every player
gets the look and sabers they chose. Slot 0 keeps reading the cvars.
==============================================================================
*/
const char *G_CoopPlayerVar( const gentity_t *ent, const char *key, const cvar_t *hostCvar )
{
	static char	buffers[8][MAX_QPATH];
	static int	next = 0;

	if ( ent && ent->s.number > 0 && ent->s.number < MAX_CLIENTS && ent->client )
	{
		char userinfo[MAX_INFO_STRING];
		gi.GetUserinfo( ent->s.number, userinfo, sizeof( userinfo ) );
		const char *v = Info_ValueForKey( userinfo, key );
		if ( v && v[0] )
		{
			char *buf = buffers[next++ & 7];
			Q_strncpyz( buf, v, MAX_QPATH );
			return buf;
		}
	}
	return hostCvar ? hostCvar->string : "";
}

// Rebuild a joiner's model/sabers when the character keys in its userinfo change
// after it spawned (the flags may only reach the client after connect).
void G_CoopCheckCharacterChange( gentity_t *ent )
{
	static char	lastKeys[MAX_CLIENTS][MAX_INFO_STRING];
	static const char *keys[] = { "g_char_model", "g_char_skin", "g_char_skin_head", "g_char_skin_torso", "g_char_skin_legs",
		"g_char_color_red", "g_char_color_green", "g_char_color_blue", "g_saber", "g_saber2", "g_saber_color", "g_saber2_color", "snd" };

	if ( !ent || ent->s.number <= 0 || ent->s.number >= MAX_CLIENTS || !ent->client )
	{
		return;
	}
	char userinfo[MAX_INFO_STRING], now[MAX_INFO_STRING] = "";
	gi.GetUserinfo( ent->s.number, userinfo, sizeof( userinfo ) );
	for ( size_t i = 0; i < ARRAY_LEN( keys ); i++ )
	{
		Q_strcat( now, sizeof( now ), Info_ValueForKey( userinfo, keys[i] ) );
		Q_strcat( now, sizeof( now ), "|" );
	}
	if ( !strcmp( now, lastKeys[ent->s.number] ) )
	{
		return;
	}
	const qboolean hadKeys = (qboolean)( lastKeys[ent->s.number][0] != '\0' );
	Q_strncpyz( lastKeys[ent->s.number], now, sizeof( lastKeys[0] ) );
	if ( hadKeys && ent->inuse && ent->health > 0 && ent->client->pers.connected == CON_CONNECTED )
	{
		G_InitPlayerFromCvars( ent );
	}
}

/*
==============================================================================
Mission objectives

ICARUS updates the host's client->sess.mission_objectives; joiners read them
back from a configstring (one character per objective: display bit + status)
into their own placeholder client so the datapad and the HUD flash work.
==============================================================================
*/
void G_CoopUpdateObjectives( void )
{
	static char	last[MAX_MISSION_OBJ + 1];
	char		now[MAX_MISSION_OBJ + 1];
	const gclient_t *host = g_entities[0].client;

	if ( !host || G_CoopNumPlayers() < 2 )
	{
		return;
	}
	for ( int i = 0; i < MAX_MISSION_OBJ; i++ )
	{
		const objectives_t *o = &host->sess.mission_objectives[i];
		now[i] = (char)( 'A' + ( o->display ? 8 : 0 ) + ( o->status & 7 ) );
	}
	now[MAX_MISSION_OBJ] = '\0';
	if ( strcmp( now, last ) )
	{
		Q_strncpyz( last, now, sizeof( last ) );
		gi.SetConfigstring( CS_COOP_OBJECTIVES, now );
	}
}

/*
==============================================================================
Sounds played straight into the host's sound system

"Bypass network for sounds on specific channels" (g_utils.cpp): dialogue,
NPC voices and a few others never become entity events, so a remote client
would stay silent (and its lips still, since facial animation follows the
voice volume). Forward them as a "snd" server command to every remote client.
==============================================================================
*/
extern qboolean CG_TryPlayCustomSound( vec3_t origin, int entityNum, soundChannel_t channel, const char *soundName, int customSoundSet );

/*
================
G_CoopChunks

CG_Chunks is a direct game -> cgame call: the debris of a func_breakable, a
misc_model_breakable or a shattered wall only ever appeared on the host.
Spawn the chunks locally as before and broadcast the same parameters as an
unreliable temp entity for the remote clients (cg_event.cpp EV_COOP_CHUNKS):
  origin = origin, origin2 = normal, angles = mins, angles2 = maxs,
  time = speed, eventParm = numChunks, weapon = material,
  modelindex = custom chunk model (CS_MODELS), time2 = scale x 100,
  modelindex2 = custom sound (CS_SOUNDS), otherEntityNum = owner
================
*/
extern void CG_Chunks( int owner, vec3_t origin, const vec3_t normal, const vec3_t mins, const vec3_t maxs,
						float speed, int numChunks, material_t chunkType, int customChunk, float baseScale, int customSound );
void G_CoopChunks( int owner, vec3_t origin, const vec3_t normal, const vec3_t mins, const vec3_t maxs,
						float speed, int numChunks, material_t chunkType, int customChunk, float baseScale, int customSound )
{
	CG_Chunks( owner, origin, normal, mins, maxs, speed, numChunks, chunkType, customChunk, baseScale, customSound );
	if ( G_CoopNumPlayers() < 2 || chunkType == MAT_NONE )
	{
		return;
	}
	gentity_t *te = G_TempEntity( origin, EV_COOP_CHUNKS );
	te->s.otherEntityNum = owner;
	VectorCopy( normal, te->s.origin2 );
	VectorCopy( mins, te->s.angles );
	VectorCopy( maxs, te->s.angles2 );
	te->s.time = (int)speed;
	te->s.eventParm = Q_min( numChunks, 255 );
	te->s.weapon = (int)chunkType;
	te->s.modelindex = customChunk;
	te->s.time2 = (int)( baseScale * 100.0f );
	te->s.modelindex2 = ( customSound > 0 && customSound < 256 ) ? customSound : 0;
}

/*
================
G_CoopMiscModelExplosion

CG_MiscModelExplosion is the same kind of game -> cgame shortcut as
CG_Chunks: the burst of a misc_model_breakable / func_breakable only ever
played on the host. Play it locally and send the parameters as a PVS-bound
temp entity for the remote clients (cg_event.cpp EV_COOP_EXPLOSION).
================
*/
extern void CG_MiscModelExplosion( vec3_t mins, vec3_t maxs, int size, material_t chunkType );
void G_CoopMiscModelExplosion( gentity_t *self, int size, material_t chunkType )
{
	CG_MiscModelExplosion( self->absmin, self->absmax, size, chunkType );	// host, as before
	if ( G_CoopNumPlayers() < 2 || chunkType == MAT_NONE )
	{
		return;
	}
	vec3_t mid;
	VectorAdd( self->absmin, self->absmax, mid );
	VectorScale( mid, 0.5f, mid );
	gentity_t *te = G_TempEntity( mid, EV_COOP_EXPLOSION );
	VectorCopy( self->absmin, te->s.angles );
	VectorCopy( self->absmax, te->s.angles2 );
	te->s.eventParm = size;			// 0..2
	te->s.weapon = (int)chunkType;	// material_t < 256
}

/*
================
G_CoopGlass

func_glass shatters through cgi_R_GetBModelVerts + CG_DoGlass straight into
the host's cgame (and its sound system). Describe the pane to the remote
clients; they own the same inline models and rebuild the shards themselves
(cg_event.cpp EV_COOP_GLASS). Broadcast: a window is a landmark and the
entity is freed right after.
================
*/
void G_CoopGlass( gentity_t *self )
{
	if ( G_CoopNumPlayers() < 2 )
	{
		return;
	}
	vec3_t mid;
	VectorAdd( self->absmin, self->absmax, mid );
	VectorScale( mid, 0.5f, mid );
	gentity_t *te = G_TempEntity( mid, EV_COOP_GLASS );
	te->svFlags |= SVF_BROADCAST;
	te->s.modelindex = self->s.modelindex;
	VectorCopy( self->pos1, te->s.origin2 );
	VectorCopy( self->pos2, te->s.angles );
	te->s.time = (int)self->splashRadius;
}

/*
==============================================================================
Per-player state that stock SP keeps in the host's cgame globals

The game and cgame are one DLL on the host, so the SP code reads
cg.saberAnimLevelPending / cg.zoomMode for "the player". With several
players those globals belong to slot 0 only; joiners get their own copy here
(never saved: joiners are not part of a .sav).
==============================================================================
*/
static int	coopSaberPending[MAX_CLIENTS];
static int	coopZoomMode[MAX_CLIENTS];
static int	coopZoomHold[MAX_CLIENTS];	// level.time until which the usercmd echo of an older mode is ignored

// the stance the player asked for (bg_pmove applies it between swings)
int G_SaberPendingLevel( const gentity_t *ent )
{
	if ( !ent || ent->s.number == 0 || ent->s.number >= MAX_CLIENTS )
	{	// the host, or an NPC it controls: the cgame global as in SP
		return cg.saberAnimLevelPending;
	}
	if ( coopSaberPending[ent->s.number] <= SS_NONE && ent->client )
	{	// nothing asked yet: whatever the saber gave it
		return ent->client->ps.saberAnimLevel;
	}
	return coopSaberPending[ent->s.number];
}

void G_SaberSetPendingLevel( gentity_t *ent, int level )
{
	if ( !ent || ent->s.number == 0 || ent->s.number >= MAX_CLIENTS )
	{
		cg.saberAnimLevelPending = level;
		return;
	}
	coopSaberPending[ent->s.number] = level;
}

// what the player's screen is zoomed with: 0 none, 1 binoculars, 2 disruptor scope, 3 LA goggles
int G_CoopZoomMode( const gentity_t *ent )
{
	if ( !ent || ent->s.number == 0 || ent->s.number >= MAX_CLIENTS )
	{	// the host (and the NPC it controls, G_ControlledByPlayer): its own cgame
		return cg.zoomMode;
	}
	return coopZoomMode[ent->s.number];
}

// a host-side decision (disruptor toggle, death, knockdown, view entity): the joiner's cgame follows
void G_CoopSetZoomMode( gentity_t *ent, int mode )
{
	if ( !ent || ent->s.number == 0 || ent->s.number >= MAX_CLIENTS )
	{
		cg.zoomMode = mode;
		cg.zoomTime = cg.time;
		cg.zoomLocked = qfalse;
		return;
	}
	if ( coopZoomMode[ent->s.number] != mode )
	{
		coopZoomMode[ent->s.number] = mode;
		coopZoomHold[ent->s.number] = level.time + 700;	// usercmds already in flight still say the old mode
		gi.SendServerCommand( ent->s.number, "zoom %i", mode );
	}
}

// the joiner's cgame reports its zoom in the usercmd (binoculars/goggles toggled locally, scope echoed back)
void G_CoopReadZoomMode( gentity_t *ent, usercmd_t *ucmd )
{
	if ( !ent || !ucmd )
	{
		return;
	}
	const int mode = ( ucmd->buttons >> BUTTON_COOP_ZOOM_SHIFT ) & 3;
	ucmd->buttons &= ~BUTTON_COOP_ZOOM_MASK;
	if ( ent->s.number == 0 || ent->s.number >= MAX_CLIENTS )
	{
		return;
	}
	if ( mode != coopZoomMode[ent->s.number] && level.time >= coopZoomHold[ent->s.number] )
	{
		coopZoomMode[ent->s.number] = mode;
	}
}

// game code asking "the player's" cgame to select a weapon: a joiner's cgame is remote
void G_CoopChangeWeapon( gentity_t *ent, int wp )
{
	extern void CG_ChangeWeapon( int num );
	if ( !ent || ent->s.number == 0 || ent->s.number >= MAX_CLIENTS )
	{
		CG_ChangeWeapon( wp );
		return;
	}
	if ( wp == WP_NONE || ( ent->client && ( ent->client->ps.stats[STAT_WEAPONS] & ( 1 << wp ) ) ) )
	{
		gi.SendServerCommand( ent->s.number, "wp %i", wp );
	}
}

/*
================
G_CoopUpdateTimescale

Force Speed / Rage are bullet time for the whole party (SP design, shared
host clock): one arbiter instead of every player's ClientThink rewriting the
cvar. The slowest active power wins; the cvar goes back to 1 once nobody is
speeding or raging. Called once per frame after the client thinks; the value
reaches the joiners through CS_COOP_TIMESCALE (sv_main.cpp).
================
*/
extern float forceSpeedValue[];
extern qboolean MatrixMode;
extern cvar_t *g_timescale;
extern cvar_t *g_skippingcin;
void G_CoopUpdateTimescale( void )
{
	static float	lastTs = 1.0f;
	float			ts = 1.0f;

	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		const gentity_t *ent = &g_entities[i];
		if ( !ent->inuse || !ent->client || ent->health <= 0 )
		{
			continue;
		}
		const playerState_t *ps = &ent->client->ps;
		if ( ps->forcePowersActive & ( 1 << FP_SPEED ) )
		{
			ts = Q_min( ts, forceSpeedValue[ps->forcePowerLevel[FP_SPEED]] );
		}
		if ( ( ps->forcePowersActive & ( 1 << FP_RAGE ) ) && ps->forcePowerLevel[FP_RAGE] >= FORCE_LEVEL_2 )
		{
			ts = Q_min( ts, forceSpeedValue[ps->forcePowerLevel[FP_RAGE] - 1] );
		}
	}
	if ( ts < 1.0f )
	{	// as in SP, re-assert it every frame while a power runs
		if ( fabs( g_timescale->value - ts ) > 0.001f )
		{
			gi.cvar_set( "timescale", va( "%4.2f", ts ) );
		}
	}
	else if ( lastTs < 1.0f )
	{	// the last power stopped: back to 1 unless something else owns the clock now
		if ( g_timescale->value != 1.0f && !MatrixMode && !g_skippingcin->integer && !in_camera )
		{
			gi.cvar_set( "timescale", "1" );
		}
	}
	lastTs = ts;
}

void G_CoopForwardSound( int entNum, int channel, int index, const char *path, int customSet )
{
	if ( G_CoopNumPlayers() < 2 )
	{
		return;
	}
	// Indexed, non-voice sounds (weapon fire, saber swings, footsteps, movers:
	// dozens per second in a fight) travel as an unreliable temp-entity event,
	// which the snapshot delivers without the 64-command reliable window ever
	// filling up. Voice lines keep the reliable path: they are rare, must not
	// be lost (captions, lip sync) and are heard from anywhere.
	const qboolean voice = (qboolean)( channel == CHAN_VOICE || channel == CHAN_VOICE_ATTEN || channel == CHAN_VOICE_GLOBAL );
	if ( index > 0 && index < MAX_SOUNDS && !voice && entNum >= 0 && entNum < MAX_GENTITIES )
	{
		gentity_t *te = G_TempEntity( g_entities[entNum].currentOrigin, EV_COOP_SOUND );
		te->svFlags |= SVF_BROADCAST;		// audible range is not the PVS
		te->s.otherEntityNum = entNum;
		te->s.time2 = channel;
		te->s.eventParm = index;
		return;
	}
	for ( int i = 1; i < MAX_CLIENTS; i++ )
	{
		const gentity_t *cl = &g_entities[i];
		if ( cl->inuse && cl->client && cl->client->pers.connected == CON_CONNECTED )
		{
			gi.SendServerCommand( i, "snd %i %i %i %i \"%s\"", entNum, channel, index, customSet, path ? path : "" );
		}
	}
}

// CG_TryPlayCustomSound for the host, forwarded to remote clients. A '*'
// name (pain, anger, taunts...) only means something against the NPC's sound
// tables, which the host alone registered (CG_RegisterNPCCustomSounds): look
// it up here and forward the file it resolved to.
extern sfxHandle_t CG_CustomSoundHandle( int entityNum, const char *soundName, int customSoundSet );
extern void cgi_S_SoundName( sfxHandle_t handle, char *buf, int buflen );
extern void cgi_S_StartSound( const vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfx );
qboolean G_CoopCustomSound( vec3_t origin, int entityNum, soundChannel_t channel, const char *soundName, int customSoundSet )
{
	if ( soundName[0] == '*' && G_CoopNumPlayers() >= 2 )
	{
		char path[MAX_QPATH];
		const sfxHandle_t handle = CG_CustomSoundHandle( entityNum, soundName, customSoundSet );
		path[0] = '\0';
		if ( handle )
		{
			cgi_S_SoundName( handle, path, sizeof( path ) );
		}
		if ( path[0] )
		{
			G_CoopForwardSound( entityNum, channel, -1, path, -1 );
		}
		if ( !handle )
		{
			return qfalse;
		}
		cgi_S_StartSound( origin, entityNum, channel, handle );
		return qtrue;
	}
	G_CoopForwardSound( entityNum, channel, -1, soundName, customSoundSet );
	return CG_TryPlayCustomSound( origin, entityNum, channel, soundName, customSoundSet );
}

// cgi_S_StartSound by path for the host, forwarded to remote clients
extern sfxHandle_t cgi_S_RegisterSound( const char *sample );
void G_CoopSoundPath( const vec3_t origin, int entityNum, int channel, const char *path )
{
	G_CoopForwardSound( entityNum, channel, -1, path, -1 );
	cgi_S_StartSound( origin, entityNum, channel, cgi_S_RegisterSound( path ) );
}
