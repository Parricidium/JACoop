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
//     model;skin;surfOff;surfOn;saber1;colors1;saber2;colors2;class;r,g,b,a
//
// (';' because JA's three-part skins already use '|') registered in
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
G_CoopClearAppearance

Entity slot is being freed or reused: forget its appearance so a later
occupant does not inherit it.
================
*/
void G_CoopClearAppearance( const gentity_t *ent )
{
	memset( &coopAppearance[ent->s.number], 0, sizeof( coopAppearance[0] ) );
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

	if ( !ent->client || !a->modelName[0] )
	{
		return;
	}

	Com_sprintf( spec, sizeof( spec ), "%s;%s;%s;%s", a->modelName, a->customSkin, a->surfOff, a->surfOn );
	G_CoopAppendSaber( spec, sizeof( spec ), &ent->client->ps.saber[0] );
	G_CoopAppendSaber( spec, sizeof( spec ), ent->client->ps.dualSabers ? &ent->client->ps.saber[1] : NULL );
	Q_strcat( spec, sizeof( spec ), va( ";%i", (int)ent->client->NPC_class ) );
	const byte *rgba = ent->client->renderInfo.customRGBA;
	Q_strcat( spec, sizeof( spec ), va( ";%i,%i,%i,%i", rgba[0], rgba[1], rgba[2], rgba[3] ) );

	if ( !strcmp( spec, a->lastSpec ) )
	{
		return;
	}
	Q_strncpyz( a->lastSpec, spec, sizeof( a->lastSpec ) );
	ent->s.modelindex3 = G_FindConfigstringIndex( spec, CS_COOP_MODELSPECS, MAX_COOP_MODELSPECS, qtrue );
}

/*
==============================================================================
Player queries for the AI and triggers

Stock SP hardcodes "the player" as g_entities[0]. With several clients the AI
must consider all of them; these helpers replace those sites.
==============================================================================
*/

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
		if ( aliveOnly && ent->health <= 0 )
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

static int				coopRespawnTime[MAX_CLIENTS];
static playerState_t	coopDeathState[MAX_CLIENTS];	// loadout snapshot taken at death

// true while this player is dead and waiting for a co-op respawn
qboolean G_CoopRespawnPending( const gentity_t *ent )
{
	return (qboolean)( G_CoopIsPlayer( ent ) && coopRespawnTime[ent->s.number] != 0 );
}

static gentity_t *G_CoopLivingTeammate( const gentity_t *self )
{
	gentity_t	*best = NULL;
	float		bestDist = -1;

	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( ent == self || !ent->inuse || !ent->client || ent->client->pers.connected != CON_CONNECTED || ent->health <= 0 )
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
	if ( !G_CoopIsPlayer( self ) || !G_CoopLivingTeammate( self ) )
	{
		return qfalse;
	}
	coopDeathState[self->s.number] = self->client->ps;
	coopRespawnTime[self->s.number] = level.time + COOP_RESPAWN_DELAY;
	gi.SendServerCommand( -1, "print \"%s ^3est tombé, retour dans %i s...\n\"", self->client->pers.netname, COOP_RESPAWN_DELAY / 1000 );
	return qtrue;
}

extern void G_DisplaceSpawnOrigin( vec3_t origin );
extern void G_AddWeaponModels( gentity_t *ent );

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
	if ( mate )
	{
		vec3_t origin;
		VectorCopy( mate->currentOrigin, origin );
		origin[2] += 9;
		G_DisplaceSpawnOrigin( origin );
		VectorCopy( origin, ps->origin );
		VectorCopy( origin, ent->currentOrigin );
		SetClientViewAngle( ent, mate->client->ps.viewangles );
		ps->eFlags ^= EF_TELEPORT_BIT;
		gi.linkentity( ent );
	}
	ent->health = ps->stats[STAT_HEALTH] = ps->stats[STAT_MAX_HEALTH];
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
		coopRespawnTime[slot] = 0;
		gentity_t *ent = &g_entities[slot];
		if ( ent->inuse && ent->client && ent->client->pers.connected == CON_CONNECTED && ent->health <= 0 )
		{
			G_CoopRespawn( ent );
		}
	}
}
