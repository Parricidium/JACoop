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

// cg_coop.cpp -- remote-client side of co-op: rebuilding characters.
//
// On the host the cgame renders every character out of its server gentity
// (ghoul2 model, sabers, bone animations set by PM_SetAnim in the server sim).
// A serverless remote client has zeroed gentities, so here we rebuild each
// character from what the network carries:
//
//   - s.modelindex3      -> CS_COOP_MODELSPECS spec string, built by g_coop.cpp:
//                           model;skin;surfOff;surfOn;saber1;colors1;saber2;colors2;class;r,g,b,a
//   - s.weapon           -> attached weapon / saber hilt models
//   - s.legsAnim/torsoAnim (+timers) -> ghoul2 bone animations, through the
//                           same PM_SetAnimFinal the host uses
//   - s.saberActive      -> blade lengths
//
// Everything here is a no-op on the host (cg_remoteClient is false there).

#include "cg_headers.h"
#include "cg_media.h"
#include "../game/anims.h"
#include "../game/wp_saber.h"

extern qboolean ValidAnimFileIndex( int index );
extern void G_SetG2PlayerModel( gentity_t * const ent, const char *modelName, const char *customSkin, const char *surfOff, const char *surfOn );
// (WP_SaberAddG2SaberModels is declared in wp_saber.h)
extern void G_CreateG2AttachedWeaponModel( gentity_t *ent, const char *psWeaponModel, int boltNum, int weaponNum );
extern void G_RemoveWeaponModels( gentity_t *ent );
extern int WP_SaberInitBladeData( gentity_t *ent );

typedef struct coopCharState_s {
	int		specIndex;		// modelindex3 the current ghoul2 was built from (0 = none)
	int		weapon;			// s.weapon the attached weapon models match
	int		legsTimer;		// last networked timers, to detect a restarted anim
	int		torsoTimer;
} coopCharState_t;

static coopCharState_t	coopChar[MAX_GENTITIES];

/*
================
CG_CoopTearDown

Forget the ghoul2 we built for this slot (entity freed on the host, or the
slot now holds something else).
================
*/
static void CG_CoopTearDown( centity_t *cent )
{
	gentity_t *gent = cent->gent;

	if ( gent->ghoul2.size() )
	{
		gi.G2API_CleanGhoul2Models( gent->ghoul2 );
	}
	gent->playerModel = -1;
	gent->weaponModel[0] = gent->weaponModel[1] = -1;
	if ( gent->client )
	{
		gent->client->clientInfo.infoValid = qfalse;
	}
	memset( &coopChar[cent->currentState.number], 0, sizeof( coopChar[0] ) );
}

/*
================
CG_CoopSplitSpec

Split "a;b;c" into fields in place. Returns the number of fields.
================
*/
static int CG_CoopSplitSpec( char *spec, const char **fields, int maxFields )
{
	int n = 0;
	char *p = spec;

	while ( n < maxFields )
	{
		fields[n++] = p;
		char *bar = strchr( p, ';' );
		if ( !bar )
		{
			break;
		}
		*bar = '\0';
		p = bar + 1;
	}
	for ( int i = n; i < maxFields; i++ )
	{
		fields[i] = "";
	}
	return n;
}

static void CG_CoopSetupSaber( gentity_t *gent, int saberNum, const char *saberName, const char *colors )
{
	saberInfo_t *saber = &gent->client->ps.saber[saberNum];

	if ( !saberName[0] )
	{
		return;
	}
	WP_SaberParseParms( saberName, saber );
	// blade colours, comma separated, in blade order
	int blade = 0;
	const char *p = colors;
	while ( *p && blade < MAX_BLADES )
	{
		saber->blade[blade++].color = (saber_colors_t)atoi( p );
		const char *comma = strchr( p, ',' );
		if ( !comma )
		{
			break;
		}
		p = comma + 1;
	}
}

/*
================
CG_CoopEnsureCharacter

Build (or rebuild, when the spec changed) the ghoul2 for a networked player
or NPC. Called for every packet entity each frame; cheap once built.
================
*/
static void CG_CoopEnsureCharacter( centity_t *cent )
{
	gentity_t			*gent = cent->gent;
	const int			entNum = cent->currentState.number;
	coopCharState_t		*st = &coopChar[entNum];

	if ( cent->currentState.eType != ET_PLAYER )
	{
		if ( st->specIndex )
		{
			CG_CoopTearDown( cent );
		}
		return;
	}

	const int specIndex = cent->currentState.modelindex3;
	if ( specIndex <= 0 || specIndex >= MAX_COOP_MODELSPECS )
	{
		return;
	}
	if ( specIndex == st->specIndex && gent->playerModel >= 0 )
	{
		return;
	}

	char spec[MAX_STRING_CHARS];
	Q_strncpyz( spec, CG_ConfigString( CS_COOP_MODELSPECS + specIndex ), sizeof( spec ) );
	if ( !spec[0] )
	{
		return;
	}
	const char *f[11];
	CG_CoopSplitSpec( spec, f, 11 );
	const char *modelName = f[0], *skin = f[1], *surfOff = f[2], *surfOn = f[3];

	if ( st->specIndex )
	{
		CG_CoopTearDown( cent );
	}

	// NPCs have no client on the remote side; players got one in GetCGameAPI
	if ( !gent->client )
	{
		gent->client = (gclient_t *)G_Alloc( sizeof( gclient_t ) );
		if ( !gent->client )
		{
			return;
		}
		memset( gent->client, 0, sizeof( gclient_t ) );
	}
	gent->s.number = entNum;
	gent->inuse = qtrue;
	gent->client->ps.clientNum = entNum;
	gent->client->NPC_class = (class_t)atoi( f[8] );
	// clothing tint (customRGBA is the whole-model colour for the jedi_* player models)
	{
		int r = 255, g = 255, b = 255, a = 255;
		if ( f[9][0] )
		{
			sscanf( f[9], "%i,%i,%i,%i", &r, &g, &b, &a );
		}
		gent->client->renderInfo.customRGBA[0] = r;
		gent->client->renderInfo.customRGBA[1] = g;
		gent->client->renderInfo.customRGBA[2] = b;
		gent->client->renderInfo.customRGBA[3] = a;
	}
	gent->playerModel = -1;
	gent->weaponModel[0] = gent->weaponModel[1] = -1;

	G_SetG2PlayerModel( gent, modelName, skin[0] ? skin : NULL, surfOff[0] ? surfOff : NULL, surfOn[0] ? surfOn : NULL );
	if ( gent->playerModel < 0 )
	{
		return;
	}

	CG_CoopSetupSaber( gent, 0, f[4], f[5] );
	CG_CoopSetupSaber( gent, 1, f[6], f[7] );
	gent->client->ps.dualSabers = (qboolean)( f[6][0] != '\0' );
	WP_SaberInitBladeData( gent );

	st->specIndex = specIndex;
	st->weapon = -1;		// force the weapon models to be (re)attached
	if ( cg_developer.integer )
	{
		Com_Printf( "coop: ent %i built model '%s' skin '%s' class %s rgba %s -> playerModel %i animFile %i\n", entNum, modelName, skin, f[8], f[9], gent->playerModel, gent->client->clientInfo.animFileIndex );
	}
	st->legsTimer = st->torsoTimer = 0;
	gent->client->ps.legsAnim = gent->client->ps.torsoAnim = -1;
}

/*
================
CG_CoopDriveAnim

Play the networked animation on the ghoul2 skeleton through the same
PM_SetAnimFinal the host's server sim uses, so blends, speeds and backwards
anims match. Idempotent per frame: PM_SetAnimFinal skips when the bone is
already playing exactly this anim. A one-shot anim the host restarted shows
up as its timer jumping back up, which we turn into SETANIM_FLAG_RESTART.
================
*/
static void CG_CoopDriveAnim( centity_t *cent )
{
	gentity_t		*gent = cent->gent;
	coopCharState_t	*st = &coopChar[cent->currentState.number];
	playerState_t	*ps = &gent->client->ps;
	const entityState_t *s = &cent->currentState;

	if ( !ValidAnimFileIndex( gent->client->clientInfo.animFileIndex ) || !gi.G2API_HaveWeGhoul2Models( gent->ghoul2 ) )
	{
		return;
	}

	// PM_SetAnimFinal scales walk/run anims by the entity's actual speed
	gent->resultspeed = sqrtf( s->pos.trDelta[0]*s->pos.trDelta[0] + s->pos.trDelta[1]*s->pos.trDelta[1] );

	const int flags = SETANIM_FLAG_OVERRIDE;
	if ( s->legsAnim > 0 && s->legsAnim < MAX_ANIMATIONS )
	{
		const int legsFlags = flags | ( ( s->legsAnimTimer > st->legsTimer && s->legsAnim == ps->legsAnim ) ? SETANIM_FLAG_RESTART : 0 );
		PM_SetAnimFinal( &ps->torsoAnim, &ps->legsAnim, SETANIM_LEGS, s->legsAnim, legsFlags, &ps->torsoAnimTimer, &ps->legsAnimTimer, gent );
	}
	// torso 0 means "no separate torso anim": leave the lower_lumbar bone following the root
	if ( s->torsoAnim > 0 && s->torsoAnim < MAX_ANIMATIONS )
	{
		const int torsoFlags = flags | ( ( s->torsoAnimTimer > st->torsoTimer && s->torsoAnim == ps->torsoAnim ) ? SETANIM_FLAG_RESTART : 0 );
		PM_SetAnimFinal( &ps->torsoAnim, &ps->legsAnim, SETANIM_TORSO, s->torsoAnim, torsoFlags, &ps->torsoAnimTimer, &ps->legsAnimTimer, gent );
	}
	st->legsTimer = s->legsAnimTimer;
	st->torsoTimer = s->torsoAnimTimer;
	// keep the placeholder ps timers in step with the network (the HUD/anim
	// code reads them through the gentity on the host path)
	ps->legsAnimTimer = s->legsAnimTimer;
	ps->torsoAnimTimer = s->torsoAnimTimer;
}

/*
================
CG_CoopSyncCharacter

Per-frame sync of the networked state into the placeholder gentity the
render code reads: velocity/ground for leg yaw and lean, weapon models,
saber blade lengths, and the skeleton animation. Called from CG_Player on
the remote client only.
================
*/
void CG_CoopSyncCharacter( centity_t *cent )
{
	if ( !cg_remoteClient )
	{
		return;
	}
	gentity_t		*gent = cent->gent;
	coopCharState_t	*st = &coopChar[cent->currentState.number];
	const entityState_t *s = &cent->currentState;

	if ( !gent || !gent->client || gent->playerModel < 0 )
	{
		return;
	}

	VectorCopy( s->pos.trDelta, gent->client->ps.velocity );
	gent->client->ps.groundEntityNum = s->groundEntityNum;
	gent->client->ps.eFlags = s->eFlags;
	gent->s.eFlags = s->eFlags;
	gent->client->ps.weapon = s->weapon;
	gent->client->ps.saberInFlight = s->saberInFlight;
	VectorCopy( cent->lerpOrigin, gent->currentOrigin );
	VectorCopy( cent->lerpAngles, gent->currentAngles );

	// weapon / saber hilt models follow s.weapon
	if ( s->weapon != st->weapon )
	{
		G_RemoveWeaponModels( gent );
		if ( s->weapon == WP_SABER )
		{
			WP_SaberAddG2SaberModels( gent, -1 );
		}
		else if ( s->weapon > WP_NONE && s->weapon < WP_NUM_WEAPONS && weaponData[s->weapon].weaponMdl[0] )
		{
			G_CreateG2AttachedWeaponModel( gent, weaponData[s->weapon].weaponMdl, gent->handRBolt, 0 );
		}
		st->weapon = s->weapon;
	}

	// blades ignite / retract with s.saberActive
	for ( int saberNum = 0; saberNum < MAX_SABERS; saberNum++ )
	{
		saberInfo_t *saber = &gent->client->ps.saber[saberNum];
		if ( !saber->name || !saber->name[0] )
		{
			continue;
		}
		const qboolean on = (qboolean)( s->weapon == WP_SABER && s->saberActive && !( saberNum == 0 && s->saberInFlight ) );
		for ( int b = 0; b < saber->numBlades; b++ )
		{
			bladeInfo_t *blade = &saber->blade[b];
			const float step = cg.frametime * 0.25f;	// ~160ms to full length
			blade->lengthOld = blade->length;
			if ( on )
			{
				blade->length = Q_min( blade->lengthMax, blade->length + step );
			}
			else
			{
				blade->length = Q_max( 0.0f, blade->length - step );
			}
			blade->active = on;
		}
	}

	CG_CoopDriveAnim( cent );
}

/*
================
CG_CoopSyncEntity

Called for every packet entity on the remote client, before it is rendered.
================
*/
void CG_CoopSyncEntity( centity_t *cent )
{
	if ( !cg_remoteClient || !cent->gent )
	{
		return;
	}
	CG_CoopEnsureCharacter( cent );
}

/*
================
CG_CoopSyncLocalPlayer

Remote client: the local player's placeholder gentity has no simulated
playerState, but the whole cgame reads it (force powers, weapons owned,
speed duration, ...) through g_entities[local].client->ps and the player
global. Copy the snapshot playerState in once per frame, keeping the saber
info CG_CoopEnsureCharacter built (the wire does not carry saberInfo_t).
Also publishes cg_localEntNum, the entity slot the local player occupies.
================
*/
int cg_localEntNum = 0;

void CG_CoopSyncLocalPlayer( void )
{
	if ( !cg.snap )
	{
		return;
	}
	cg_localEntNum = cg.snap->ps.clientNum;
	if ( !cg_remoteClient )
	{
		return;
	}
	// shared code compares against level.time; there is no server clock here, use ours
	level.time = cg.time;

	gentity_t *me = &g_entities[cg_localEntNum];
	if ( !me->client )
	{
		return;
	}
	// the gamecode's "player" global is read by the cgame too (force speed FOV,
	// weapon selection, datapad); on the remote client it must be us, not slot 0
	player = me;

	saberInfo_t	saber[MAX_SABERS];
	const qboolean dualSabers = me->client->ps.dualSabers;
	memcpy( saber, me->client->ps.saber, sizeof( saber ) );

	me->client->ps = cg.snap->ps;

	memcpy( me->client->ps.saber, saber, sizeof( saber ) );
	me->client->ps.dualSabers = dualSabers;

	me->s.number = cg_localEntNum;
	me->s.clientNum = cg_localEntNum;
	me->s.eFlags = cg.snap->ps.eFlags;
	me->inuse = qtrue;
	VectorCopy( cg.snap->ps.origin, me->currentOrigin );
	VectorCopy( cg.snap->ps.viewangles, me->currentAngles );
}

/*
==============================================================================
Cinematic camera on the remote client

The host broadcasts an ET_COOPCAMERA entity while it is in a cutscene camera
(g_coop.cpp). Here we feed its interpolated state into our own client_camera
and raise in_camera, so the stock camera render path (CGCam_RenderScene) and
the HUD/2D suppression behave exactly as on the host.
==============================================================================
*/

#include "cg_camera.h"

extern void CG_CalcEntityLerpPositions( centity_t *cent );

static qboolean coopCameraActive = qfalse;

void CG_CoopSyncCamera( void )
{
	if ( !cg_remoteClient || !cg.snap )
	{
		return;
	}

	centity_t *cam = NULL;
	for ( int i = 0; i < cg.snap->numEntities; i++ )
	{
		centity_t *cent = &cg_entities[cg.snap->entities[i].number];
		if ( cent->currentState.eType == ET_COOPCAMERA )
		{
			cam = cent;
			break;
		}
	}

	if ( !cam )
	{
		if ( coopCameraActive )
		{
			coopCameraActive = qfalse;
			in_camera = false;
			client_camera.info_state = 0;
			client_camera.bar_alpha = 0.0f;
			client_camera.bar_height = 0.0f;
			client_camera.fade_color[3] = 0.0f;
		}
		return;
	}

	CG_CalcEntityLerpPositions( cam );

	if ( !coopCameraActive )
	{
		coopCameraActive = qtrue;
		memset( &client_camera, 0, sizeof( client_camera ) );
		in_camera = true;
		cg.zoomMode = 0;
	}
	client_camera.info_state = 0;
	VectorCopy( cam->lerpOrigin, client_camera.origin );
	VectorCopy( cam->lerpAngles, client_camera.angles );
	client_camera.FOV = cam->currentState.origin2[0];
	client_camera.FOV2 = client_camera.FOV;
	client_camera.bar_alpha = cam->currentState.origin2[1] > 0.0f ? 1.0f : 0.0f;
	client_camera.bar_height = cam->currentState.origin2[1];
	// CGCam_UpdateBarFade snaps to the *_dest values once bar_time is stale; keep them equal
	client_camera.bar_alpha_dest = client_camera.bar_alpha_source = client_camera.bar_alpha;
	client_camera.bar_height_dest = client_camera.bar_height_source = client_camera.bar_height;
	client_camera.bar_time = cg.time;
	VectorCopy( cam->currentState.angles2, client_camera.fade_color );
	client_camera.fade_color[3] = cam->currentState.origin2[2];
}
