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
	int		lastDebugTime;
	qboolean inFlight;		// s.saberInFlight we last acted on (hand hilt removed / restored)
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
		gent->client->renderInfo.lookTarget = ENTITYNUM_NONE;	// 0 would be "stare at the host"
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
CG_CoopEnsureG2Model

Non-character entities whose model is a ghoul2 .glm (misc_model_ghoul,
turrets, det packs / trip mines, saber pickups): the host built their ghoul2
in the game, and the renderer draws nothing for a .glm handle without an
instance. Build one here from the model the entity names. Items name it in
modelindex3 (g_items.cpp), everything else in modelindex.
================
*/
static int coopG2Key[MAX_GENTITIES];	// CS_MODELS index the ghoul2 in this slot was built from (0 = none)

static void CG_CoopEnsureG2Model( centity_t *cent )
{
	gentity_t			*gent = cent->gent;
	const entityState_t	*s = &cent->currentState;
	const int			entNum = s->number;
	int					modelIndex = 0;
	int					key = 0;

	if ( s->eType == ET_ITEM )
	{
		modelIndex = s->modelindex3;
	}
	else if ( s->eType != ET_PLAYER && s->eType < ET_EVENTS && s->solid != SOLID_BMODEL )
	{
		modelIndex = s->modelindex;
	}
	if ( modelIndex > 0 && modelIndex < MAX_MODELS )
	{
		const char	*name = CG_ConfigString( CS_MODELS + modelIndex );
		const int	len = strlen( name );
		if ( len > 4 && !Q_stricmp( name + len - 4, ".glm" ) )
		{
			key = modelIndex;
		}
	}
	if ( key == coopG2Key[entNum] && ( !key || gent->ghoul2.size() ) )
	{
		return;
	}
	if ( coopChar[entNum].specIndex )
	{	// the slot held a character
		CG_CoopTearDown( cent );
	}
	else if ( coopG2Key[entNum] && gent->ghoul2.size() )
	{
		gi.G2API_CleanGhoul2Models( gent->ghoul2 );
	}
	coopG2Key[entNum] = key;
	if ( !key )
	{
		return;
	}
	const char *name = CG_ConfigString( CS_MODELS + key );
	gent->s.number = entNum;
	gent->inuse = qtrue;
	gent->playerModel = gi.G2API_InitGhoul2Model( gent->ghoul2, name, key, NULL_HANDLE, NULL_HANDLE, 0, 0 );
	if ( s->eType == ET_GENERAL && s->weapon == WP_SABER && gent->playerModel >= 0 )
	{	// a thrown saber: CG_General draws its blade from bolt 0 ("*flash") of model weaponModel[0]
		gi.G2API_AddBolt( &gent->ghoul2[gent->playerModel], "*flash" );
		gent->weaponModel[0] = gent->playerModel;
		gent->classname = "lightsaber";
	}
	if ( cg_developer.integer )
	{
		Com_Printf( "coop: ent %i built ghoul2 '%s' (type %i) -> %i\n", entNum, name, s->eType, gent->playerModel );
	}
}

/*
================
CG_CoopEnts_f

"coop_ents": what the last snapshot holds, to find an object the host sees
and we do not.
================
*/
void CG_CoopEnts_f( void )
{
	if ( !cg.snap )
	{
		return;
	}
	for ( int i = 0; i < cg.snap->numEntities; i++ )
	{
		const entityState_t	*es = &cg.snap->entities[i];
		const centity_t		*cent = &cg_entities[es->number];
		const char			*name;
		if ( es->eType == ET_ITEM )
		{
			name = ( es->modelindex > 0 && es->modelindex < bg_numItems ) ? bg_itemlist[es->modelindex].classname : "?";
		}
		else if ( es->solid == SOLID_BMODEL )
		{
			name = va( "*%i (draw %i) pos %i base %.0f %.0f %.0f lerpOrg %.0f %.0f %.0f interp %i curState.number %i apos %i lerpAng %.0f %.0f %.0f", es->modelindex, cgs.inlineDrawModel[es->modelindex],
				es->pos.trType, es->pos.trBase[0], es->pos.trBase[1], es->pos.trBase[2], cent->lerpOrigin[0], cent->lerpOrigin[1], cent->lerpOrigin[2],
				cent->interpolate, cent->currentState.number, es->apos.trType, cent->lerpAngles[0], cent->lerpAngles[1], cent->lerpAngles[2] );
		}
		else if ( es->eType == ET_PLAYER )
		{
			name = va( "spec %i", es->modelindex3 );
		}
		else
		{
			name = CG_ConfigString( CS_MODELS + es->modelindex );
		}
		Com_Printf( "%4i type %2i model %3i %-40s ghoul2 %i%s%s at %.0f %.0f %.0f %s\n", es->number, es->eType, es->modelindex, name,
			cent->gent ? (int)cent->gent->ghoul2.size() : -1, ( es->eFlags & EF_NODRAW ) ? " NODRAW" : "", ( es->eFlags & EF_PERMANENT ) ? " PERMANENT" : "",
			es->pos.trBase[0], es->pos.trBase[1], es->pos.trBase[2], ( cent->gent && cent->gent->classname ) ? cent->gent->classname : "" );
	}
	Com_Printf( "%i entities in the snapshot\n", cg.snap->numEntities );
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
	if ( cg_developer.integer > 1 && cent->currentState.number < MAX_CLIENTS && ( cg.time / 1000 ) != ( st->lastDebugTime / 1000 ) )
	{
		float cur = 0, spd = 0; int sf = 0, ef = 0, fl = 0;
		const qboolean playing = gi.G2API_GetBoneAnimIndex( &gent->ghoul2[gent->playerModel], gent->rootBone, cg.time, &cur, &sf, &ef, &fl, &spd, NULL );
		const animation_t *anims = level.knownAnimFileSets[gent->client->clientInfo.animFileIndex].animations;
		Com_Printf( "coop: ent %i anim net %i/%i ps %i/%i root bone %i playing %i speed %.2f frames %i-%i animFile %i glaOffset %i animGla %i numFrames %i vel %.0f at %.0f %.0f %.0f\n", cent->currentState.number, s->legsAnim, s->torsoAnim, ps->legsAnim, ps->torsoAnim, gent->rootBone, playing, spd, sf, ef, gent->client->clientInfo.animFileIndex, gi.G2API_GetAnimIndex( &gent->ghoul2[gent->playerModel] ), anims[s->legsAnim].glaIndex, anims[s->legsAnim].numFrames, gent->resultspeed, cent->lerpOrigin[0], cent->lerpOrigin[1], cent->lerpOrigin[2] );
		st->lastDebugTime = cg.time;
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
	if ( cent->currentState.number != cg_localEntNum )
	{	// ours comes from the playerState (CG_CoopSyncLocalPlayer)
		gent->health = s->coopHealth;
		gent->max_health = s->coopMaxHealth;
	}
	gent->client->renderInfo.lookMode = LM_ENT;
	gent->client->renderInfo.lookTarget = ( s->coopLookTarget >= 0 && s->coopLookTarget < ENTITYNUM_WORLD ) ? s->coopLookTarget : ENTITYNUM_NONE;
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
		st->inFlight = qfalse;
	}

	// saber throw: the hilt leaves the hand (the thrown entity carries it and
	// its blade, drawn from our saber[0] data), and comes back on the catch
	if ( s->weapon == WP_SABER && (qboolean)( s->saberInFlight != 0 ) != st->inFlight )
	{
		st->inFlight = (qboolean)( s->saberInFlight != 0 );
		if ( st->inFlight )
		{
			if ( gent->weaponModel[0] > 0 )
			{
				gi.G2API_RemoveGhoul2Model( gent->ghoul2, gent->weaponModel[0] );
				gent->weaponModel[0] = -1;
			}
		}
		else
		{
			WP_SaberAddG2SaberModels( gent, 0 );
		}
	}

	// blades ignite / retract with s.saberActive
	for ( int saberNum = 0; saberNum < MAX_SABERS; saberNum++ )
	{
		saberInfo_t *saber = &gent->client->ps.saber[saberNum];
		if ( !saber->name || !saber->name[0] )
		{
			continue;
		}
		const qboolean on = (qboolean)( s->weapon == WP_SABER && s->saberActive );
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
	// the placeholder stands for an entity that exists on the host; CG_Item
	// & co skip a gentity that is not "inuse" (every pickup was invisible)
	cent->gent->inuse = qtrue;
	cent->gent->s.number = cent->currentState.number;
	CG_CoopEnsureG2Model( cent );
	CG_CoopEnsureCharacter( cent );
	if ( cent->currentState.eType == ET_GENERAL && cent->currentState.weapon == WP_SABER )
	{	// thrown saber: the blade is drawn from its owner's saber data (CG_General)
		const int owner = cent->currentState.otherEntityNum;
		cent->gent->owner = ( owner >= 0 && owner < MAX_CLIENTS && g_entities[owner].client ) ? &g_entities[owner] : NULL;
	}
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


	// mission objectives mirrored from the host (CS_COOP_OBJECTIVES); a change
	// flashes the HUD prompt exactly as an ICARUS update does on the host
	{
		extern qboolean missionInfo_Updated;
		const char *obj = CG_ConfigString( CS_COOP_OBJECTIVES );
		for ( int i = 0; i < MAX_MISSION_OBJ && obj[i]; i++ )
		{
			objectives_t *o = &me->client->sess.mission_objectives[i];
			const int code = obj[i] - 'A';
			const qboolean display = (qboolean)( ( code & 8 ) != 0 );
			const int status = code & 7;
			if ( o->display != display || o->status != status )
			{
				o->display = display;
				o->status = status;
				missionInfo_Updated = qtrue;
			}
		}
	}

	me->s.number = cg_localEntNum;
	me->s.clientNum = cg_localEntNum;
	me->s.eFlags = cg.snap->ps.eFlags;
	me->health = cg.snap->ps.stats[STAT_HEALTH];
	me->max_health = cg.snap->ps.stats[STAT_MAX_HEALTH];
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

// The camera only reaches us at the snapshot rate (sv_fps, 20 Hz), and our
// clock sits right at the newest snapshot, so following it directly steps
// and stalls. Keep the last samples and render the camera cg_coopCameraLag
// ms in the past, between two of them.
#define COOP_CAM_SAMPLES	32
typedef struct coopCamSample_s {
	int			time;
	vec3_t		origin;
	vec3_t		angles;
	float		fov;
	float		bar;
	vec4_t		fade;
	qboolean	cut;		// a jump from the previous sample: never interpolate into it
} coopCamSample_t;
static coopCamSample_t	coopCam[COOP_CAM_SAMPLES];
static int				coopCamCount;		// samples in the ring
static int				coopCamHead;		// next slot to write
static int				coopCamLastTime;	// serverTime of the newest sample

static const coopCamSample_t *CG_CoopCamAt( int i )	// 0 = oldest
{
	return &coopCam[( coopCamHead - coopCamCount + i + COOP_CAM_SAMPLES ) % COOP_CAM_SAMPLES];
}

static void CG_CoopCamPush( const entityState_t *s, int time )
{
	if ( coopCamCount && time <= coopCamLastTime )
	{
		return;
	}
	coopCamSample_t *c = &coopCam[coopCamHead];
	c->time = time;
	VectorCopy( s->pos.trBase, c->origin );
	VectorCopy( s->apos.trBase, c->angles );
	c->fov = s->origin2[0];
	c->bar = s->origin2[1];
	VectorCopy( s->angles2, c->fade );
	c->fade[3] = s->origin2[2];
	c->cut = qfalse;
	if ( coopCamCount )
	{
		const coopCamSample_t *p = CG_CoopCamAt( coopCamCount - 1 );
		const int dt = time - p->time;
		if ( dt > 500 || Distance( p->origin, c->origin ) > 400.0f
			|| fabsf( AngleSubtract( p->angles[YAW], c->angles[YAW] ) ) > 60.0f
			|| fabsf( AngleSubtract( p->angles[PITCH], c->angles[PITCH] ) ) > 60.0f )
		{
			c->cut = qtrue;
		}
	}
	coopCamHead = ( coopCamHead + 1 ) % COOP_CAM_SAMPLES;
	if ( coopCamCount < COOP_CAM_SAMPLES )
	{
		coopCamCount++;
	}
	coopCamLastTime = time;
}

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
		coopCamCount = coopCamHead = coopCamLastTime = 0;
		return;
	}

	// new samples: this snapshot's, and the next one's when we already have it
	CG_CoopCamPush( &cam->currentState, cg.snap->serverTime );
	if ( cg.nextSnap && cam->interpolate && cam->nextState )
	{
		CG_CoopCamPush( cam->nextState, cg.nextSnap->serverTime );
	}

	// the two samples around our (delayed) render time
	const int t = cg.time - cg_coopCameraLag.integer;
	const coopCamSample_t *a = CG_CoopCamAt( 0 ), *b = a;
	for ( int i = 1; i < coopCamCount; i++ )
	{
		b = CG_CoopCamAt( i );
		if ( b->time >= t )
		{
			break;
		}
		a = b;
	}
	float f = 0.0f;
	if ( b->cut )
	{
		f = ( t >= b->time ) ? 1.0f : 0.0f;
	}
	else if ( b->time > a->time && t > a->time )
	{
		f = Com_Clamp( 0.0f, 1.0f, ( t - a->time ) / (float)( b->time - a->time ) );
	}

	if ( !coopCameraActive )
	{
		coopCameraActive = qtrue;
		memset( &client_camera, 0, sizeof( client_camera ) );
		in_camera = true;
		cg.zoomMode = 0;
	}
	client_camera.info_state = 0;
	for ( int i = 0; i < 3; i++ )
	{
		client_camera.origin[i] = a->origin[i] + f * ( b->origin[i] - a->origin[i] );
		client_camera.angles[i] = LerpAngle( a->angles[i], b->angles[i], f );
		client_camera.fade_color[i] = a->fade[i] + f * ( b->fade[i] - a->fade[i] );
	}
	client_camera.FOV = a->fov + f * ( b->fov - a->fov );
	client_camera.FOV2 = client_camera.FOV;
	const float bar = a->bar + f * ( b->bar - a->bar );
	client_camera.bar_alpha = bar > 0.0f ? 1.0f : 0.0f;
	client_camera.bar_height = bar;
	// CGCam_UpdateBarFade snaps to the *_dest values once bar_time is stale; keep them equal
	client_camera.bar_alpha_dest = client_camera.bar_alpha_source = client_camera.bar_alpha;
	client_camera.bar_height_dest = client_camera.bar_height_source = client_camera.bar_height;
	client_camera.bar_time = cg.time;
	client_camera.fade_color[3] = a->fade[3] + f * ( b->fade[3] - a->fade[3] );
	if ( cg_developer.integer > 1 )
	{
		Com_Printf( "coopcam %i t %i a %i b %i f %.2f n %i org %.1f %.1f %.1f yaw %.1f%s\n", cg.time, t, a->time, b->time, f, coopCamCount, client_camera.origin[0], client_camera.origin[1], client_camera.origin[2], client_camera.angles[YAW], b->cut ? " cut" : "" );
	}
}

/*
==============================================================================
Client-only map entities

SP_misc_model_static "cheats since this is SP": the host pushes the model
straight into its cgame (CG_CreateMiscEntFromGent) and frees the entity, so
nothing ever reaches the wire. A remote client re-reads them from the map's
entity string itself, before CG_CreateMiscEnts registers the queue.
==============================================================================
*/
extern void CG_CreateMiscEnt( const char *model, const vec3_t origin, const vec3_t angles, const vec3_t scale, float zOff );

void CG_CoopSpawnStaticModels( void )
{
	if ( !cg_remoteClient || !gi.CoopEntityString )
	{
		return;
	}
	const char *p = gi.CoopEntityString();
	int count = 0;

	COM_BeginParseSession();

	while ( p && *p )
	{
		const char *token = COM_Parse( &p );
		if ( !token[0] || token[0] != '{' )
		{
			break;
		}

		char	classname[64] = "", model[MAX_QPATH] = "";
		vec3_t	origin = { 0, 0, 0 }, angles = { 0, 0, 0 }, scale = { 1, 1, 1 };
		float	zOff = 0;

		while ( 1 )
		{
			char key[MAX_TOKEN_CHARS];
			token = COM_Parse( &p );
			if ( !token[0] || token[0] == '}' )
			{
				break;
			}
			Q_strncpyz( key, token, sizeof( key ) );
			token = COM_Parse( &p );
			if ( !token[0] )
			{
				break;
			}
			if ( !Q_stricmp( key, "classname" ) )		Q_strncpyz( classname, token, sizeof( classname ) );
			else if ( !Q_stricmp( key, "model" ) )		Q_strncpyz( model, token, sizeof( model ) );
			else if ( !Q_stricmp( key, "origin" ) )		sscanf( token, "%f %f %f", &origin[0], &origin[1], &origin[2] );
			else if ( !Q_stricmp( key, "angles" ) )		sscanf( token, "%f %f %f", &angles[0], &angles[1], &angles[2] );
			else if ( !Q_stricmp( key, "angle" ) )		angles[YAW] = atof( token );
			else if ( !Q_stricmp( key, "modelscale_vec" ) )	sscanf( token, "%f %f %f", &scale[0], &scale[1], &scale[2] );
			else if ( !Q_stricmp( key, "modelscale" ) )	{ const float s = atof( token ); if ( s != 0.0f ) { scale[0] = scale[1] = scale[2] = s; } }
			else if ( !Q_stricmp( key, "zoffset" ) )	zOff = atof( token );
		}

		if ( !Q_stricmp( classname, "misc_model_static" ) && model[0] )
		{
			CG_CreateMiscEnt( model, origin, angles, scale, zOff );
			count++;
		}
	}
	COM_EndParseSession();
	if ( cg_developer.integer )
	{
		Com_Printf( "coop: %i misc_model_static spawned from the entity string\n", count );
	}
}

/*
==============================================================================
CG_CoopPrecacheCharacters

The host built every character's ghoul2 during its own level load, so the
appearance specs are all in the gamestate by the time we connect. Building
each model once now (and throwing the scratch ghoul2 away) pulls the .glm,
skins, textures and animation.cfg into the caches, instead of stalling the
first frame each character comes into view (the stall was long enough to
trigger "Delta request from out of date entities" on the host).
==============================================================================
*/
void CG_CoopPrecacheCharacters( void )
{
	if ( !cg_remoteClient )
	{
		return;
	}
	static gentity_t	scratch;
	static gclient_t	scratchClient;
	char				seen[MAX_COOP_MODELSPECS][MAX_QPATH];
	int					numSeen = 0, count = 0;

	for ( int i = 1; i < MAX_COOP_MODELSPECS; i++ )
	{
		char spec[MAX_STRING_CHARS];
		Q_strncpyz( spec, CG_ConfigString( CS_COOP_MODELSPECS + i ), sizeof( spec ) );
		if ( !spec[0] )
		{
			continue;
		}
		const char *f[11];
		CG_CoopSplitSpec( spec, f, 11 );
		// one build per model+skin pair is enough, the rest is per-entity
		char key[MAX_QPATH];
		Com_sprintf( key, sizeof( key ), "%s/%s", f[0], f[1] );
		int k;
		for ( k = 0; k < numSeen; k++ )
		{
			if ( !Q_stricmp( seen[k], key ) )
			{
				break;
			}
		}
		if ( k < numSeen )
		{
			continue;
		}
		Q_strncpyz( seen[numSeen++], key, sizeof( seen[0] ) );

		scratch.s.number = ENTITYNUM_NONE;	// keeps G_CoopRecordModel off the real slots
		scratch.client = &scratchClient;
		scratch.playerModel = -1;
		scratch.weaponModel[0] = scratch.weaponModel[1] = -1;
		G_SetG2PlayerModel( &scratch, f[0], f[1][0] ? f[1] : NULL, f[2][0] ? f[2] : NULL, f[3][0] ? f[3] : NULL );
		if ( scratch.playerModel >= 0 )
		{
			count++;
		}
		gi.G2API_CleanGhoul2Models( scratch.ghoul2 );
		scratch.playerModel = -1;
	}
	if ( cg_developer.integer )
	{
		Com_Printf( "coop: %i character models precached\n", count );
	}
}

/*
================
CG_CoopFixLocalEntityState

CG_AddPacketEntities rebuilds the local player's entityState from the
playerState every frame (PlayerStateToEntityState). Fields the playerState
does not carry over the wire — saber ignition, the appearance spec, anim
timers — end up zeroed, so the joiner's own blade and model spec vanish.
The server also sends our own entity in the snapshot; take those fields back
from it.
================
*/
void CG_CoopFixLocalEntityState( centity_t *cent )
{
	if ( !cg_remoteClient || !cg.snap )
	{
		return;
	}
	VectorCopy( cg.snap->ps.velocity, cent->currentState.pos.trDelta );	// PM_SetAnimFinal scales walk/run anims by it
	for ( int i = 0; i < cg.snap->numEntities; i++ )
	{
		const entityState_t *es = &cg.snap->entities[i];
		if ( es->number != cent->currentState.number )
		{
			continue;
		}
		cent->currentState.saberActive = es->saberActive;
		cent->currentState.saberInFlight = es->saberInFlight;
		cent->currentState.modelindex3 = es->modelindex3;
		cent->currentState.legsAnimTimer = es->legsAnimTimer;
		cent->currentState.torsoAnimTimer = es->torsoAnimTimer;
		cent->currentState.coopHealth = es->coopHealth;
		cent->currentState.coopMaxHealth = es->coopMaxHealth;
		cent->currentState.coopLookTarget = es->coopLookTarget;
		return;
	}
}

/*
================
CG_CoopMissionFailed_f

"mf <statusTextIndex>": everyone is dead, or a script failed the mission.
Same screen as the host; it goes away with the host's next level load.
================
*/
void CG_CoopMissionFailed_f( void )
{
	extern int statusTextIndex;

	statusTextIndex = atoi( CG_Argv( 1 ) );
	cg.missionStatusShow = qtrue;
}

/*
================
CG_CoopMenu_f

"coopmenu <name>": the host wants this menu on our screen (the lobby).
================
*/
void CG_CoopMenu_f( void )
{
	cgi_UI_SetActive_Menu( (char *)CG_Argv( 1 ) );
}

/*
================
CG_CoopSound_f

"snd <ent> <channel> <index> <customSet> <path>" from the host: a sound the
host played straight into its own sound system (G_SoundOnEnt & co).
================
*/
extern qboolean CG_TryPlayCustomSound( vec3_t origin, int entityNum, soundChannel_t channel, const char *soundName, int customSoundSet );

void CG_CoopSound_f( void )
{
	const int	entNum = atoi( CG_Argv( 1 ) );
	const int	channel = atoi( CG_Argv( 2 ) );
	const int	index = atoi( CG_Argv( 3 ) );
	const int	customSet = atoi( CG_Argv( 4 ) );
	const char	*path = CG_Argv( 5 );

	if ( entNum < 0 || entNum >= MAX_GENTITIES )
	{
		return;
	}
	cgi_S_UpdateEntityPosition( entNum, cg_entities[entNum].lerpOrigin );
	if ( index > 0 && index < MAX_SOUNDS && cgs.sound_precache[index] )
	{
		cgi_S_StartSound( NULL, entNum, channel, cgs.sound_precache[index] );
	}
	else if ( path[0] )
	{
		if ( customSet >= 0 )
		{
			CG_TryPlayCustomSound( NULL, entNum, (soundChannel_t)channel, path, customSet );
		}
		else
		{
			cgi_S_StartSound( NULL, entNum, channel, cgi_S_RegisterSound( path ) );
		}
	}
}
