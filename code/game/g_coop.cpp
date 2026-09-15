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
//     model;skin;surfOff;surfOn;saber1;colors1;saber2;colors2;class
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

	if ( !strcmp( spec, a->lastSpec ) )
	{
		return;
	}
	Q_strncpyz( a->lastSpec, spec, sizeof( a->lastSpec ) );
	ent->s.modelindex3 = G_FindConfigstringIndex( spec, CS_COOP_MODELSPECS, MAX_COOP_MODELSPECS, qtrue );
}
