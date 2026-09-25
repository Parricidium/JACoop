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
//                           model;skin;surfOff;surfOn;saber1;colors1;saber2;colors2;class;r,g,b,a;hand
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
#include "../game/g_vehicles.h"
#include "../ghoul2/ghoul2_gore.h"		// CRagDollParams, CRagDollUpdateParams (le ragdoll des corps)

extern qboolean ValidAnimFileIndex( int index );
extern void G_SetG2PlayerModel( gentity_t * const ent, const char *modelName, const char *customSkin, const char *surfOff, const char *surfOn );
// (WP_SaberAddG2SaberModels is declared in wp_saber.h)
extern void G_CreateG2AttachedWeaponModel( gentity_t *ent, const char *psWeaponModel, int boltNum, int weaponNum );
extern void G_RemoveWeaponModels( gentity_t *ent );
extern int WP_SaberInitBladeData( gentity_t *ent );
extern void CG_RegisterClientRenderInfo( clientInfo_t *ci, renderInfo_t *ri );

typedef struct coopCharState_s {
	int		specIndex;		// modelindex3 the current ghoul2 was built from (0 = none)
	int		weapon;			// s.weapon the attached weapon models match
	int		legsTimer;		// last networked timers, to detect a restarted anim
	int		torsoTimer;
	int		lastDebugTime;
	qboolean inFlight;		// s.saberInFlight we last acted on (hand hilt removed / restored)
	qboolean legacyMd3;		// pre-ghoul2 md3 legs/torso/head model (remote_sp, mouse, seeker): no ghoul2 to build
	byte	tint[4];		// customRGBA the spec gave (debug: detect it being clobbered)
	unsigned bodyHash;		// hash of the spec's body fields (everything but the hand prop) the ghoul2 was built from
	int		handSpec;		// spec index the hand prop was last read from
	char	hand[MAX_QPATH+2];	// "L:path" / "R:path" attached (gent->cinematicModel), or ""
} coopCharState_t;

// hash of the spec's body: the first 10 fields (the 11th is the hand prop, which never needs a rebuild)
static unsigned CG_CoopBodyHash( const char *spec )
{
	unsigned h = 2166136261u;
	int seps = 0;
	for ( const char *p = spec; *p; p++ )
	{
		if ( *p == ';' && ++seps == 10 )
		{
			break;
		}
		h = ( h ^ (unsigned char)*p ) * 16777619u;
	}
	return h;
}

// the spec's 11th field (up to the next ';'), "" when absent
static const char *CG_CoopHandField( const char *spec )
{
	static char	field[MAX_QPATH + 2];
	int			seps = 0;
	for ( const char *p = spec; *p; p++ )
	{
		if ( *p == ';' && ++seps == 10 )
		{
			const char *end = strchr( p + 1, ';' );
			Q_strncpyz( field, p + 1, end ? Q_min( (int)( end - p ), (int)sizeof( field ) ) : (int)sizeof( field ) );
			return field;
		}
	}
	return "";
}

static coopCharState_t	coopChar[MAX_GENTITIES];
static qboolean			coopHostVideo = qfalse;	// the host is watching an in-game video (CG_CoopVideo_f)
// cl_coopPaksGen the skin / model tables and the characters were built at, latched
// again at every level start (CG_CoopReset) so that a pack loaded after this module
// registered its graphics is always noticed (CG_CoopCheckPaksGen)
static int				coopPaksGenSeen = -1;
static void CG_CoopResetCamera( void );

/*
================
CG_CoopTearDown

Forget the ghoul2 we built for this slot (entity freed on the host, or the
slot now holds something else).
================
*/
// --- l'etat du ragdoll des corps (voir plus bas)
#define COOP_RAG_MAX_GOALS	16
typedef struct coopRag_s {
	qboolean	on;
	qboolean	started;	// SetRagDoll fait sur notre squelette
	int			goalTime;	// derniers effecteurs recus (0 : aucun)
	int			nGoals;
	vec3_t		goals[COOP_RAG_MAX_GOALS];
} coopRag_t;

static coopRag_t	coopRag[MAX_GENTITIES];

static void CG_CoopTearDown( centity_t *cent )
{
	memset( &coopRag[cent->currentState.number], 0, sizeof( coopRag[0] ) );	// le ragdoll suit le squelette
	gentity_t *gent = cent->gent;

	if ( cg_developer.integer )
	{
		Com_Printf( "coop: ent %i torn down (type %i spec %i, %i models)\n", cent->currentState.number, cent->currentState.eType, cent->currentState.modelindex3, (int)gent->ghoul2.size() );
	}
	if ( gent->ghoul2.size() )
	{
		gi.G2API_CleanGhoul2Models( gent->ghoul2 );
	}
	gent->playerModel = -1;
	gent->weaponModel[0] = gent->weaponModel[1] = -1;
	gent->cinematicModel = -1;
	if ( gent->client )
	{
		gent->client->clientInfo.infoValid = qfalse;
	}
	memset( &coopChar[cent->currentState.number], 0, sizeof( coopChar[0] ) );
}

/*
================
CG_CoopModelSpecChanged

The host rewrote a CS_COOP_MODELSPECS slot (G_CoopModelSpecIndex recycling):
every character built from it must be rebuilt from the new string.
================
*/
void CG_CoopModelSpecChanged( int specIndex )
{
	if ( !cg_remoteClient || specIndex <= 0 )
	{
		return;
	}
	for ( int i = 0; i < MAX_GENTITIES; i++ )
	{
		if ( coopChar[i].specIndex == specIndex )
		{
			coopChar[i].specIndex = -1;		// never matches: CG_CoopEnsureCharacter tears down and rebuilds
		}
	}
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

extern void WP_SaberLoadParms( void );

static void CG_CoopSetupSaber( gentity_t *gent, int saberNum, const char *saberName, const char *colors )
{
	saberInfo_t *saber = &gent->client->ps.saber[saberNum];

	if ( !saberName[0] )
	{
		return;
	}
	const qboolean known = WP_SaberParseParms( saberName, saber );
	if ( cg_developer.integer )
	{	// the hilt may come from a pack we downloaded: say whether we know it now
		Com_Printf( "coop: ent %i saber%i '%s'%s model '%s' colours '%s'\n", gent->s.number, saberNum + 1, saberName,
			known ? "" : " (INCONNU)", saber->model ? saber->model : "", colors );
	}
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
	if ( specIndex == st->specIndex && st->legacyMd3 && gent->client && gent->client->clientInfo.infoValid )
	{	// legacy md3 model registered
		return;
	}
	const qboolean built = (qboolean)( gent->playerModel >= 0
		&& gent->playerModel < gent->ghoul2.size() && strstr( gent->ghoul2[gent->playerModel].mFileName, "models/players/" ) != NULL );
	if ( specIndex == st->specIndex && built )
	{	// built, and the skeleton slot still holds the body
		return;
	}
	if ( st->specIndex && built && CG_CoopBodyHash( CG_ConfigString( CS_COOP_MODELSPECS + specIndex ) ) == st->bodyHash )
	{	// only the hand prop changed: CG_CoopSyncHandModel attaches it, the body stays
		st->specIndex = specIndex;
		return;
	}
	if ( cg_developer.integer && st->specIndex )
	{
		Com_Printf( "coop: ent %i rebuilding: spec %i->%i playerModel %i slot '%s' of %i models weaponModel %i/%i\n", entNum, st->specIndex, specIndex, gent->playerModel,
			( gent->playerModel >= 0 && gent->playerModel < gent->ghoul2.size() ) ? gent->ghoul2[gent->playerModel].mFileName : "?", (int)gent->ghoul2.size(), gent->weaponModel[0], gent->weaponModel[1] );
	}

	char spec[MAX_STRING_CHARS];
	Q_strncpyz( spec, CG_ConfigString( CS_COOP_MODELSPECS + specIndex ), sizeof( spec ) );
	if ( !spec[0] )
	{
		return;
	}
	const unsigned bodyHash = CG_CoopBodyHash( spec );
	const char *f[12];
	CG_CoopSplitSpec( spec, f, 12 );
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
	gent->s.clientNum = cent->currentState.clientNum;
	gent->inuse = qtrue;
	gent->client->ps.clientNum = entNum;
	gent->client->NPC_class = (class_t)atoi( f[8] );
	// a vehicle: the render code reads its Vehicle_t (muzzle tags, armor low
	// effect, flying flag) and G_IsRidingVehicle returns it for the rider,
	// so hold a placeholder with the .veh info, looked up by name
	if ( gent->client->NPC_class == CLASS_VEHICLE && f[11][0] )
	{
		const int vIndex = BG_VehicleGetIndex( f[11] );
		if ( vIndex > VEHICLE_NONE && vIndex < MAX_VEHICLES )
		{
			if ( !gent->m_pVehicle )
			{
				gent->m_pVehicle = (Vehicle_t *)G_Alloc( sizeof( Vehicle_t ) );
			}
			if ( gent->m_pVehicle )
			{
				memset( gent->m_pVehicle, 0, sizeof( Vehicle_t ) );
				gent->m_pVehicle->m_pParentEntity = gent;
				gent->m_pVehicle->m_pVehicleInfo = &g_vehicleInfo[vIndex];
				for ( int i = 0; i < MAX_VEHICLE_MUZZLES; i++ )
				{
					gent->m_pVehicle->m_iMuzzleTag[i] = -1;
				}
				if ( cg_developer.integer )
				{
					Com_Printf( "coop: ent %i vehicle '%s' (index %i)\n", entNum, f[11], vIndex );
				}
			}
		}
	}
	else
	{
		gent->m_pVehicle = NULL;
	}
	if ( gent->client->NPC_class == CLASS_VEHICLE && !gent->m_pVehicle )
	{
		Com_Printf( "coop: ent %i vehicle '%s' unknown here, not built\n", entNum, f[11] );
		return;
	}
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
	gent->cinematicModel = -1;
	st->bodyHash = bodyHash;
	st->handSpec = 0;
	st->hand[0] = '\0';

	if ( modelName[0] == '@' )
	{	// "@legs;torso;head": pre-ghoul2 md3 model, drawn by CG_Player's legacy path from clientInfo
		renderInfo_t *ri = &gent->client->renderInfo;
		Q_strncpyz( ri->legsModelName, modelName + 1, sizeof( ri->legsModelName ) );
		Q_strncpyz( ri->torsoModelName, skin, sizeof( ri->torsoModelName ) );
		Q_strncpyz( ri->headModelName, surfOff, sizeof( ri->headModelName ) );
		ri->legsFpsMod = ri->torsoFpsMod = 1.0f;
		CG_RegisterClientRenderInfo( &gent->client->clientInfo, ri );
		gent->client->clientInfo.infoValid = qtrue;
		st->specIndex = specIndex;
		st->legacyMd3 = qtrue;
		st->weapon = -1;
		if ( cg_developer.integer )
		{
			Com_Printf( "coop: ent %i built legacy model legs '%s' torso '%s' head '%s' class %s -> legs %i torso %i head %i\n", entNum, modelName + 1, skin, surfOff, f[8],
				gent->client->clientInfo.legsModel, gent->client->clientInfo.torsoModel, gent->client->clientInfo.headModel );
		}
		return;
	}

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
		const int skinIdx = gent->ghoul2[gent->playerModel].mCustomSkin;
		Com_Printf( "coop: ent %i built model '%s' skin '%s' class %s rgba %s -> playerModel %i animFile %i skinIdx %i cs '%s' handle %i glm '%s'\n", entNum, modelName, skin, f[8], f[9], gent->playerModel, gent->client->clientInfo.animFileIndex,
			skinIdx, ( skinIdx > 0 && skinIdx < MAX_CHARSKINS ) ? CG_ConfigString( CS_CHARSKINS + skinIdx ) : "?", ( skinIdx > 0 && skinIdx < MAX_CHARSKINS ) ? cgs.skins[skinIdx] : -1,
			gent->ghoul2[gent->playerModel].mFileName );
		Com_Printf( "coop: ent %i tint now %i,%i,%i,%i\n", entNum, gent->client->renderInfo.customRGBA[0], gent->client->renderInfo.customRGBA[1], gent->client->renderInfo.customRGBA[2], gent->client->renderInfo.customRGBA[3] );
	}
	memcpy( st->tint, gent->client->renderInfo.customRGBA, 4 );
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
static int coopG2Key[MAX_GENTITIES];	// CS_MODELS index the ghoul2 in this slot was built from (0 = none, -1 = a limb)
static int coopMatrixKey[MAX_GENTITIES];	// s.time of the matrix effect this slot is thinking for (0 = none)

// Emplaced guns / E-Webs / turrets: CG_General animates the gun from host-only
// fields (activator, health, bolts, bounceCount); the host networks the user
// (s.otherEntityNum), the health (s.coopHealth) and the turret pitch (s.angles2)
typedef enum { COOP_GUN_NONE, COOP_GUN_EWEB, COOP_GUN_CHAIR, COOP_GUN_TURRET } coopGunKind_t;
static byte		coopGunKind[MAX_GENTITIES];
static float	coopTurretPitch[MAX_GENTITIES][2];	// last angles2 applied

// Dismemberment: the host cuts the victim's ghoul2 into a limb entity
// (ET_THINKER) and turns the limb's surfaces off on the victim - all of it in
// ghoul2 data that never travels. The host sends "limb" with the names it
// used (g_combat.cpp G_Dismember); when that entity reaches the snapshot the
// same cut is made here from the victim's placeholder.
typedef struct coopLimb_s {
	qboolean	pending;
	int			owner;
	char		limbName[MAX_QPATH];
	char		limbCapName[MAX_QPATH];
	char		stubCapName[MAX_QPATH];
	char		rotateBone[MAX_QPATH];
	int			limbAnim;
	int			protocolWaist;
} coopLimb_t;
static coopLimb_t coopLimb[MAX_GENTITIES];

void CG_CoopLimb_f( void )
{
	const int num = atoi( CG_Argv( 1 ) );
	if ( !cg_remoteClient || num <= 0 || num >= MAX_GENTITIES )
	{
		return;
	}
	coopLimb_t *l = &coopLimb[num];
	memset( l, 0, sizeof( *l ) );
	l->pending = qtrue;
	l->owner = atoi( CG_Argv( 2 ) );
	Q_strncpyz( l->limbName, CG_Argv( 3 ), sizeof( l->limbName ) );
	Q_strncpyz( l->limbCapName, CG_Argv( 4 ), sizeof( l->limbCapName ) );
	Q_strncpyz( l->stubCapName, CG_Argv( 5 ), sizeof( l->stubCapName ) );
	Q_strncpyz( l->rotateBone, CG_Argv( 6 ), sizeof( l->rotateBone ) );
	l->limbAnim = atoi( CG_Argv( 7 ) );
	l->protocolWaist = atoi( CG_Argv( 8 ) );
}

extern void CG_Limb( centity_t *cent );

// the limb entity is in the snapshot and its victim has a body: cut it
static void CG_CoopBuildLimb( centity_t *cent )
{
	const int	entNum = cent->currentState.number;
	coopLimb_t	*l = &coopLimb[entNum];
	gentity_t	*limb = cent->gent;

	if ( l->owner < 0 || l->owner >= MAX_GENTITIES )
	{
		l->pending = qfalse;
		return;
	}
	gentity_t *owner = &g_entities[l->owner];
	if ( !owner->client || owner->playerModel < 0 || owner->playerModel >= owner->ghoul2.size() )
	{
		return;	// victim not built yet, try next frame
	}
	if ( limb->ghoul2.size() )
	{
		gi.G2API_CleanGhoul2Models( limb->ghoul2 );
	}
	gi.G2API_CopyGhoul2Instance( owner->ghoul2, limb->ghoul2, 0 );
	limb->playerModel = 0;
	limb->craniumBone = owner->craniumBone;
	limb->cervicalBone = owner->cervicalBone;
	limb->thoracicBone = owner->thoracicBone;
	limb->upperLumbarBone = owner->upperLumbarBone;
	limb->lowerLumbarBone = owner->lowerLumbarBone;
	limb->hipsBone = owner->hipsBone;
	limb->rootBone = owner->rootBone;
	gi.G2API_StopBoneAnimIndex( &limb->ghoul2[0], limb->hipsBone );
	gi.G2API_SetRootSurface( limb->ghoul2, 0, l->limbName );
	if ( l->protocolWaist && ValidAnimFileIndex( owner->client->clientInfo.animFileIndex ) )
	{
		gi.G2API_StopBoneAnim( &limb->ghoul2[0], "model_root" );
		gi.G2API_StopBoneAnim( &limb->ghoul2[0], "motion" );
		gi.G2API_StopBoneAnim( &limb->ghoul2[0], "pelvis" );
		gi.G2API_StopBoneAnim( &limb->ghoul2[0], "upper_lumbar" );
		animation_t *animations = level.knownAnimFileSets[owner->client->clientInfo.animFileIndex].animations;
		gi.G2API_SetBoneAnim( &limb->ghoul2[0], 0, animations[l->limbAnim].firstFrame,
			animations[l->limbAnim].numFrames + animations[l->limbAnim].firstFrame, BONE_ANIM_OVERRIDE_FREEZE, 1, cg.time, -1, -1 );
	}
	if ( l->rotateBone[0] )
	{
		gi.G2API_SetNewOrigin( &limb->ghoul2[0], gi.G2API_AddBolt( &limb->ghoul2[0], l->rotateBone ) );
	}
	if ( l->limbCapName[0] )
	{
		gi.G2API_SetSurfaceOnOff( &limb->ghoul2[0], l->limbCapName, 0 );
	}
	limb->s.number = entNum;
	limb->inuse = qtrue;
	limb->classname = "limb";
	limb->owner = owner;
	limb->target2 = G_NewString( l->limbName );
	limb->target3 = l->stubCapName[0] ? G_NewString( l->stubCapName ) : NULL;
	limb->count = l->limbAnim;
	limb->aimDebounceTime = 0;
	limb->startRGBA[0] = owner->client->renderInfo.customRGBA[0];
	limb->startRGBA[1] = owner->client->renderInfo.customRGBA[1];
	limb->startRGBA[2] = owner->client->renderInfo.customRGBA[2];
	CG_Limb( cent );	// takes the limb off the victim, caps the stub
	coopG2Key[entNum] = -1;
	l->pending = qfalse;
	if ( cg_developer.integer )
	{
		Com_Printf( "coop: ent %i limb '%s' cut from ent %i\n", entNum, l->limbName, l->owner );
	}
}

/*
================
CG_CoopReset

New level on a remote client (CG_Init): the game module is not reloaded, so
the placeholder gentities, their ghoul2 (built against the previous level's
renderer handles and sound/skin indices) and our tables would survive. Drop
everything; the snapshot rebuilds it.
================
*/
static int			coopDownMax;		// downed overlay: the bleed-out total, taken from the first frame down
static char			coopSoundSetName[MAX_COOP_SOUNDSETS][64];	// CS_COOP_SOUNDSETS, as gentity soundSet pointers
static qboolean		coopAllDownShown;	// the everyone-down screen is up: no overlay under it

void CG_CoopReset( void )
{
	// CG_PreInit: whatever the pack list is right now is what CG_RegisterGraphics
	// is about to register the skins and models against. A pack that arrives after
	// this (the joiner reconnects and FS_CoopHavePak loads a leftover coopdl_*.pk3
	// once the host's offer comes in, well after this module has registered its
	// graphics) then always shows up as a generation change.
	coopPaksGenSeen = cg_coopPaksGen.integer;
	coopAllDownShown = qfalse;
	coopDownMax = 0;
	memset( coopLimb, 0, sizeof( coopLimb ) );
	if ( !cg_remoteClient )
	{
		return;
	}
	for ( int i = 0; i < MAX_GENTITIES; i++ )
	{
		gentity_t *gent = &g_entities[i];
		if ( gent->ghoul2.size() )
		{
			gi.G2API_CleanGhoul2Models( gent->ghoul2 );
		}
		gent->playerModel = -1;
		gent->weaponModel[0] = gent->weaponModel[1] = -1;
		gent->owner = NULL;
		gent->m_pVehicle = NULL;
		gent->inuse = qfalse;
		gent->e_clThinkFunc = clThinkF_NULL;
		gent->soundSet = NULL;
		gent->setTime = 0;
		if ( gent->client )
		{
			memset( gent->client->ps.saber, 0, sizeof( gent->client->ps.saber ) );
			gent->client->ps.dualSabers = qfalse;
			gent->client->clientInfo.infoValid = qfalse;
		}
	}
	memset( coopChar, 0, sizeof( coopChar ) );
	memset( coopG2Key, 0, sizeof( coopG2Key ) );
	coopHostVideo = qfalse;
	cgi_Cvar_Set( "skippingCinematic", "0" );
	cgi_Cvar_Set( "timescale", "1" );
	CG_CoopResetCamera();
	memset( coopMatrixKey, 0, sizeof( coopMatrixKey ) );
	{
		extern int coopMatrixEnt;
		extern qboolean MatrixMode;
		coopMatrixEnt = -1;
		MatrixMode = qfalse;
	}
	memset( coopGunKind, 0, sizeof( coopGunKind ) );
	memset( coopTurretPitch, 0, sizeof( coopTurretPitch ) );
	memset( coopSoundSetName, 0, sizeof( coopSoundSetName ) );
	Com_Printf( "coop: remote client state reset for the new level\n" );
}

/*
================
CG_CoopSyncSoundSet

Local ambient sets (target_speaker & co): the host tags each emitter with its
CS_COOP_SOUNDSETS slot in s.time2 (G_ParsePrecaches); the placeholder gets the
name so CG_AddLocalSet plays it here like on the host.
================
*/
static void CG_CoopSyncSoundSet( centity_t *cent )
{
	const entityState_t	*s = &cent->currentState;
	gentity_t			*gent = cent->gent;
	const int			idx = s->time2;

	if ( s->eType == ET_MOVER || s->eType == ET_PLAYER || s->eType == ET_ITEM || idx <= 0 || idx >= MAX_COOP_SOUNDSETS )
	{
		if ( gent->soundSet )
		{
			gent->soundSet = NULL;
			gent->setTime = 0;
		}
		return;
	}
	if ( !coopSoundSetName[idx][0] )
	{
		const char *name = CG_ConfigString( CS_COOP_SOUNDSETS + idx );
		if ( !name[0] )
		{
			return;
		}
		Q_strncpyz( coopSoundSetName[idx], name, sizeof( coopSoundSetName[idx] ) );
	}
	if ( gent->soundSet != coopSoundSetName[idx] )
	{
		gent->soundSet = coopSoundSetName[idx];
		gent->setTime = 0;
		if ( cg_developer.integer )
		{
			Com_Printf( "coop: ent %i local sound set '%s'\n", s->number, gent->soundSet );
		}
	}
}

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
	if ( s->eType == ET_THINKER && ( coopLimb[entNum].pending || coopG2Key[entNum] == -1 ) )
	{	// a dismembered limb (built from the "limb" command, kept while the entity lives)
		if ( coopLimb[entNum].pending )
		{
			CG_CoopBuildLimb( cent );
		}
		if ( coopG2Key[entNum] == -1 )
		{
			return;
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
		if ( cg_developer.integer )
		{
			Com_Printf( "coop: ent %i object ghoul2 cleaned (key %i -> %i, type %i)\n", entNum, coopG2Key[entNum], key, s->eType );
		}
		gi.G2API_CleanGhoul2Models( gent->ghoul2 );
		// a character built later in this slot must start from scratch (its
		// body would otherwise be believed present, and sabers land in slot 0)
		gent->playerModel = -1;
		gent->weaponModel[0] = gent->weaponModel[1] = -1;
		memset( &coopChar[entNum], 0, sizeof( coopChar[0] ) );
	}
	coopG2Key[entNum] = key;
	coopGunKind[entNum] = COOP_GUN_NONE;
	// a slot reused by another thing: nothing of the gun / item it was stays behind
	gent->s.weapon = WP_NONE;
	gent->health = gent->max_health = 0;
	gent->bounceCount = 0;
	gent->cinematicModel = -1;
	if ( gent->activator && gent->activator->owner == gent )
	{
		gent->activator->owner = NULL;
	}
	gent->activator = NULL;
	if ( !key )
	{
		return;
	}
	const char *name = CG_ConfigString( CS_MODELS + key );
	gent->s.number = entNum;
	gent->inuse = qtrue;
	gent->playerModel = gi.G2API_InitGhoul2Model( gent->ghoul2, name, key, NULL_HANDLE, NULL_HANDLE, 0, 0 );
	if ( gent->playerModel >= 0 && s->eType == ET_GENERAL )
	{	// the bolts and bones CG_General's gun code needs (SP_emplaced_eweb / SP_emplaced_gun / turrets)
		// (CG_Player aims the gun through lowerLumbarBone/upperLumbarBone and seats
		// the user on headBolt: same bones and bolts as SP_emplaced_eweb / SP_emplaced_gun)
		if ( strstr( name, "eweb_model" ) )
		{
			gent->handLBolt = gi.G2API_AddBolt( &gent->ghoul2[gent->playerModel], "*cannonflash" );
			gent->handRBolt = -1;
			gent->headBolt = gi.G2API_AddBolt( &gent->ghoul2[gent->playerModel], "cannon_Xrot" );
			gent->rootBone = gi.G2API_GetBoneIndex( &gent->ghoul2[gent->playerModel], "model_root", qtrue );
			gent->lowerLumbarBone = gi.G2API_GetBoneIndex( &gent->ghoul2[gent->playerModel], "cannon_Yrot", qtrue );
			gent->upperLumbarBone = gi.G2API_GetBoneIndex( &gent->ghoul2[gent->playerModel], "cannon_Xrot", qtrue );
			gi.G2API_SetBoneAnglesIndex( &gent->ghoul2[gent->playerModel], gent->lowerLumbarBone, vec3_origin, BONE_ANGLES_POSTMULT, POSITIVE_Z, NEGATIVE_X, NEGATIVE_Y, NULL, 0, 0 );
			gi.G2API_SetBoneAnglesIndex( &gent->ghoul2[gent->playerModel], gent->upperLumbarBone, vec3_origin, BONE_ANGLES_POSTMULT, POSITIVE_Z, NEGATIVE_X, NEGATIVE_Y, NULL, 0, 0 );
			gent->bounceCount = 1;
			coopGunKind[entNum] = COOP_GUN_EWEB;
			if ( cg_developer.integer )
			{
				Com_Printf( "coop: ent %i E-Web bolts flash %i seat %i bones root %i yrot %i xrot %i\n", entNum, gent->handLBolt, gent->headBolt, gent->rootBone, gent->lowerLumbarBone, gent->upperLumbarBone );
			}
		}
		else if ( strstr( name, "turret_chair" ) )
		{
			gent->headBolt = gi.G2API_AddBolt( &gent->ghoul2[gent->playerModel], "*seat" );
			gent->handLBolt = gi.G2API_AddBolt( &gent->ghoul2[gent->playerModel], "*flash01" );
			gent->handRBolt = gi.G2API_AddBolt( &gent->ghoul2[gent->playerModel], "*flash02" );
			gent->rootBone = gi.G2API_GetBoneIndex( &gent->ghoul2[gent->playerModel], "base_bone", qtrue );
			gent->lowerLumbarBone = gi.G2API_GetBoneIndex( &gent->ghoul2[gent->playerModel], "swivel_bone", qtrue );
			gent->upperLumbarBone = -1;
			gi.G2API_SetBoneAnglesIndex( &gent->ghoul2[gent->playerModel], gent->lowerLumbarBone, vec3_origin, BONE_ANGLES_POSTMULT, POSITIVE_Y, POSITIVE_Z, POSITIVE_X, NULL, 0, 0 );
			gent->bounceCount = 0;
			coopGunKind[entNum] = COOP_GUN_CHAIR;
		}
		else if ( strstr( name, "turret_canon" ) || strstr( name, "laser_cannon_model" ) )
		{
			coopGunKind[entNum] = COOP_GUN_TURRET;
			coopTurretPitch[entNum][0] = coopTurretPitch[entNum][1] = 0;
		}
	}
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
float coopLastMouseSpeed = -1.0f, coopLastMouseFov = -1.0f, coopLastMouseTs = -1.0f;
vec3_t cg_coopCrosshairPos;	// cg_draw.cpp : where the reticle sits in the world
extern bool in_camera;

void CG_CoopEnts_f( void )
{
	if ( !cg.snap )
	{
		return;
	}
	Com_Printf( "coop: timescale %.2f cl_paused %i in_camera %i fov_y %.1f mouseSpeed %.2f (fov %.1f ts %.2f at that point) viewEntity %i zoom %i\n",
		cg_timescale.value, cg_paused.integer, in_camera ? 1 : 0, cg.refdef.fov_y, coopLastMouseSpeed, coopLastMouseFov, coopLastMouseTs, cg.snap->ps.viewEntity, cg.zoomMode );
	{
		const playerState_t *ps = &cg.snap->ps;
		char levels[NUM_FORCE_POWERS * 2 + 1];
		for ( int fp = 0; fp < NUM_FORCE_POWERS; fp++ )
		{
			levels[fp * 2] = (char)( '0' + ps->forcePowerLevel[fp] );
			levels[fp * 2 + 1] = ( fp == NUM_FORCE_POWERS - 1 ) ? '\0' : ' ';
		}
		Com_Printf( "coop: ps weapon %i weapons 0x%x force %i/%i known 0x%x levels [%s] styles 0x%x stance %i active 0x%x speedDur %i drainEnt %i view %.0f %.0f\n",
			ps->weapon, ps->stats[STAT_WEAPONS], ps->forcePower, ps->forcePowerMax, ps->forcePowersKnown, levels, ps->saberStylesKnown, ps->saberAnimLevel,
			ps->forcePowersActive, ps->forcePowerDuration[FP_SPEED], ps->forceDrainEntityNum, ps->viewangles[PITCH], ps->viewangles[YAW] );
		Com_Printf( "coop: reticule en %.0f %.0f %.0f (3e personne %i), joueur en %.0f %.0f %.0f\n", cg_coopCrosshairPos[0], cg_coopCrosshairPos[1], cg_coopCrosshairPos[2], cg.renderingThirdPerson,
			ps->origin[0], ps->origin[1], ps->origin[2] );
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
			vec3_t amb, dir, ldir;
			cgi_R_GetLighting( cent->lerpOrigin, amb, dir, ldir );
			name = va( "spec %i hp %i/%i%s pw 0x%x ef 0x%x force 0x%x shock %i push %i light amb %.0f %.0f %.0f dir %.0f %.0f %.0f", es->modelindex3, es->coopHealth, es->coopMaxHealth & COOP_MAXHEALTH_MASK,
				( es->coopMaxHealth & COOP_MAXHEALTH_MORELIGHT ) ? " morelight" : "", es->powerups, es->eFlags,
				es->coopForce, es->coopShockTime, es->coopPushTime, amb[0], amb[1], amb[2], dir[0], dir[1], dir[2] );
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
/*
===============
JACoop : le ragdoll des corps chez l'invite

L'hote dit quand un corps entre en ragdoll ("ragon") et quand il se pose
("ragoff") ; entre les deux, l'invite simule le meme ragdoll sur son
squelette local (memes parametres que le pilote du jeu), et, si l'hote
envoie la position des effecteurs ("rag", g_coopRagdollSync), les suit :
la pose finit par etre la meme partout.
===============
*/
static const char	*coopRagEffectors[] =
{
	"rhand", "lhand", "rtibia", "ltibia", "rtalus", "ltalus", "rradiusX", "lradiusX", "rfemurX", "lfemurX",
	"rhumerusX", "lhumerusX", "thoracic", "ceyebrow", "pelvis", NULL
};

class CCoopRagUpdateParams : public CRagDollUpdateParams
{
	void EffectorCollision( const SRagDollEffectorCollision &data ) {}
	void RagDollBegin() {}
	virtual void RagDollSettled() {}
	void Collision() {}
#ifdef _DEBUG
	void DebugLine( vec3_t p1, vec3_t p2, int color, bool bbox ) {}
#endif
};

void CG_CoopRagOn_f( void )
{
	const int n = atoi( CG_Argv( 1 ) );

	if ( !cg_remoteClient || n <= 0 || n >= MAX_GENTITIES )
	{
		return;
	}
	coopRag[n].on = qtrue;
	coopRag[n].started = qfalse;
	coopRag[n].goalTime = 0;
	coopRag[n].nGoals = 0;
	cgi_Cvar_Set( "broadsword", "1" );		// le moteur refuse SetRagDoll sinon
	if ( gi.Cvar_VariableIntegerValue( "developer" ) ) Com_Printf( "coop: rag on ent %i (invite)\n", n );
}

void CG_CoopRagOff_f( void )
{
	const int n = atoi( CG_Argv( 1 ) );

	if ( !cg_remoteClient || n <= 0 || n >= MAX_GENTITIES )
	{
		return;
	}
	if ( coopRag[n].started && cg_entities[n].gent && gi.G2API_HaveWeGhoul2Models( cg_entities[n].gent->ghoul2 ) )
	{
		gi.G2API_ResetRagDoll( cg_entities[n].gent->ghoul2 );
	}
	if ( gi.Cvar_VariableIntegerValue( "developer" ) ) Com_Printf( "coop: rag off ent %i (invite), demarre %i, effecteurs recus %i\n", n, (int)coopRag[n].started, coopRag[n].nGoals );
	memset( &coopRag[n], 0, sizeof( coopRag[n] ) );
}

void CG_CoopRag_f( void )
{
	const int n = atoi( CG_Argv( 1 ) );
	const int argc = cgi_Argc();

	if ( !cg_remoteClient || n <= 0 || n >= MAX_GENTITIES || !coopRag[n].on )
	{
		return;
	}
	coopRag_t *r = &coopRag[n];

	r->nGoals = ( argc - 2 ) / 3;
	if ( r->nGoals > COOP_RAG_MAX_GOALS ) r->nGoals = COOP_RAG_MAX_GOALS;
	for ( int i = 0; i < r->nGoals; i++ )
	{
		r->goals[i][0] = (float)atoi( CG_Argv( 2 + i * 3 ) );
		r->goals[i][1] = (float)atoi( CG_Argv( 3 + i * 3 ) );
		r->goals[i][2] = (float)atoi( CG_Argv( 4 + i * 3 ) );
	}
	r->goalTime = cg.time;
	{
		static int said;

		if ( said++ < 3 )
		{
			if ( gi.Cvar_VariableIntegerValue( "developer" ) ) Com_Printf( "coop: rag paquet recu ent %i, %i effecteurs (invite)\n", n, r->nGoals );
		}
	}
}

// qtrue : le ragdoll pilote les os, l'animation normale ne doit pas y toucher
static qboolean CG_CoopRagdollDrive( centity_t *cent )
{
	const int	n = cent->currentState.number;
	coopRag_t	*r = &coopRag[n];
	gentity_t	*gent = cent->gent;

	if ( !r->on || !gent || !gent->client || gent->playerModel < 0 || gent->playerModel >= gent->ghoul2.size() )
	{
		return qfalse;
	}
	const int afi = gent->client->clientInfo.animFileIndex;

	if ( !ValidAnimFileIndex( afi ) )
	{
		return qfalse;
	}
	int anim = cent->currentState.legsAnim;

	if ( anim < 0 || anim >= MAX_ANIMATIONS || level.knownAnimFileSets[afi].animations[anim].numFrames <= 0 )
	{
		anim = BOTH_DEATH1;
	}
	const animation_t	*a = &level.knownAnimFileSets[afi].animations[anim];
	vec3_t				ang;

	VectorSet( ang, 0, cent->lerpAngles[YAW], 0 );
	if ( !r->started )
	{
		CRagDollParams	tParms;
		float			currentFrame;
		int				startFrame, endFrame, flags;
		float			animSpeed;

		tParms.startFrame = a->firstFrame;
		tParms.endFrame = a->firstFrame + a->numFrames;
		if ( gi.G2API_GetBoneAnim( &gent->ghoul2[0], "model_root", cg.time, &currentFrame, &startFrame, &endFrame, &flags, &animSpeed, NULL ) )
		{	// figer l'anim sur son image, comme le pilote du jeu
			gi.G2API_SetBoneAnim( &gent->ghoul2[0], "lower_lumbar", currentFrame, currentFrame + 1, flags, animSpeed, cg.time, currentFrame, 500 );
			gi.G2API_SetBoneAnim( &gent->ghoul2[0], "model_root", currentFrame, currentFrame + 1, flags, animSpeed, cg.time, currentFrame, 500 );
			gi.G2API_SetBoneAnim( &gent->ghoul2[0], "Motion", currentFrame, currentFrame + 1, flags, animSpeed, cg.time, currentFrame, 500 );
		}
		gi.G2API_SetBoneAngles( &gent->ghoul2[gent->playerModel], "upper_lumbar", vec3_origin, BONE_ANGLES_POSTMULT, POSITIVE_X, NEGATIVE_Y, NEGATIVE_Z, NULL, 100, cg.time );
		gi.G2API_SetBoneAngles( &gent->ghoul2[gent->playerModel], "lower_lumbar", vec3_origin, BONE_ANGLES_POSTMULT, POSITIVE_X, NEGATIVE_Y, NEGATIVE_Z, NULL, 100, cg.time );
		gi.G2API_SetBoneAngles( &gent->ghoul2[gent->playerModel], "thoracic", vec3_origin, BONE_ANGLES_POSTMULT, POSITIVE_X, NEGATIVE_Y, NEGATIVE_Z, NULL, 100, cg.time );
		gi.G2API_SetBoneAngles( &gent->ghoul2[gent->playerModel], "cervical", vec3_origin, BONE_ANGLES_POSTMULT, POSITIVE_X, NEGATIVE_Y, NEGATIVE_Z, NULL, 100, cg.time );
		VectorCopy( ang, tParms.angles );
		VectorCopy( cent->lerpOrigin, tParms.position );
		VectorCopy( gent->s.modelScale, tParms.scale );
		tParms.me = n;
		tParms.groundEnt = cent->currentState.groundEntityNum;
		tParms.collisionType = 1;
		tParms.RagPhase = CRagDollParams::RP_DEATH_COLLISION;
		tParms.fShotStrength = 4;
		gi.G2API_SetRagDoll( gent->ghoul2, &tParms );
		r->started = qtrue;
		if ( gi.Cvar_VariableIntegerValue( "developer" ) ) Com_Printf( "coop: rag demarre ent %i anim %i frames %i-%i (invite)\n", n, anim, tParms.startFrame, tParms.endFrame );
	}

	CCoopRagUpdateParams tu;

	VectorCopy( ang, tu.angles );
	VectorCopy( cent->lerpOrigin, tu.position );
	VectorCopy( gent->s.modelScale, tu.scale );
	tu.me = n;
	tu.settleFrame = a->firstFrame + a->numFrames - 1;
	tu.groundEnt = cent->currentState.groundEntityNum;
	if ( tu.groundEnt != ENTITYNUM_NONE )
	{
		VectorClear( tu.velocity );
	}
	else
	{
		VectorScale( cent->currentState.pos.trDelta, 0.4f, tu.velocity );
	}
	gi.G2API_AnimateG2Models( gent->ghoul2, cg.time, &tu );

	if ( r->goalTime && cg.time - r->goalTime < 400 )
	{	// la pose de l'hote
		for ( int i = 0; i < r->nGoals && coopRagEffectors[i]; i++ )
		{
			gi.G2API_RagEffectorGoal( gent->ghoul2, coopRagEffectors[i], r->goals[i] );
		}
		gi.G2API_RagForceSolve( gent->ghoul2, qtrue );
	}
	else if ( r->goalTime )
	{	// plus rien depuis un moment : la simulation locale reprend seule
		for ( int i = 0; coopRagEffectors[i]; i++ )
		{
			gi.G2API_RagEffectorGoal( gent->ghoul2, coopRagEffectors[i], NULL );
		}
		gi.G2API_RagForceSolve( gent->ghoul2, qfalse );
		r->goalTime = 0;
	}
	return qtrue;
}

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
	if ( CG_CoopRagdollDrive( cent ) )
	{	// le ragdoll tient les os
		return;
	}
	if ( gent->playerModel < 0 || gent->playerModel >= gent->ghoul2.size() || !strstr( gent->ghoul2[gent->playerModel].mFileName, "models/players/" ) )
	{	// the skeleton slot does not hold a character model: animating it (anim index offsets) would break its ghoul2
		static int lastWarn = 0;
		if ( cg.time - lastWarn > 2000 )
		{
			lastWarn = cg.time;
			Com_Printf( "coop: ent %i (type %i spec %i) playerModel %i is '%s' of %i models, not animating\n", cent->currentState.number, cent->currentState.eType, cent->currentState.modelindex3,
				gent->playerModel, ( gent->playerModel >= 0 && gent->playerModel < gent->ghoul2.size() ) ? gent->ghoul2[gent->playerModel].mFileName : "?", (int)gent->ghoul2.size() );
		}
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
CG_CoopSyncHandModel

The cutscene prop bolted to a hand (spec field 11, G_CoopRecordHandModel):
(re)attach it the way Q3_AddLHandModel / Q3_AddRHandModel do on the host,
in gent->cinematicModel (the scepter beam effect reads it).
================
*/
static void CG_CoopSyncHandModel( gentity_t *gent, coopCharState_t *st )
{
	if ( st->handSpec == st->specIndex )
	{
		return;
	}
	st->handSpec = st->specIndex;
	const char *hand = CG_CoopHandField( CG_ConfigString( CS_COOP_MODELSPECS + st->specIndex ) );
	if ( !strcmp( hand, st->hand ) )
	{
		return;
	}
	if ( gent->cinematicModel > 0 && gent->cinematicModel < gent->ghoul2.size() )
	{
		gi.G2API_RemoveGhoul2Model( gent->ghoul2, gent->cinematicModel );
	}
	gent->cinematicModel = -1;
	Q_strncpyz( st->hand, hand, sizeof( st->hand ) );
	if ( ( hand[0] == 'L' || hand[0] == 'R' ) && hand[1] == ':' && hand[2] && gent->playerModel >= 0 )
	{
		const char *model = hand + 2;
		gent->cinematicModel = gi.G2API_InitGhoul2Model( gent->ghoul2, model, 0, 0, 0, 0, 0 );
		if ( gent->cinematicModel != -1 )
		{
			gi.G2API_AttachG2Model( &gent->ghoul2[gent->cinematicModel], &gent->ghoul2[gent->playerModel],
						hand[0] == 'L' ? gent->handLBolt : gent->handRBolt, gent->playerModel );
		}
	}
	if ( cg_developer.integer )
	{
		Com_Printf( "coop: ent %i hand prop '%s' -> model %i\n", gent->s.number, hand, gent->cinematicModel );
	}
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

	if ( !gent || !gent->client || ( gent->playerModel < 0 && !st->legacyMd3 ) )
	{
		return;
	}

	if ( cg_developer.integer && memcmp( st->tint, gent->client->renderInfo.customRGBA, 4 ) && st->specIndex )
	{
		Com_Printf( "coop: ent %i tint clobbered: %i,%i,%i,%i (spec had %i,%i,%i,%i)\n", s->number,
			gent->client->renderInfo.customRGBA[0], gent->client->renderInfo.customRGBA[1], gent->client->renderInfo.customRGBA[2], gent->client->renderInfo.customRGBA[3],
			st->tint[0], st->tint[1], st->tint[2], st->tint[3] );
		memcpy( st->tint, gent->client->renderInfo.customRGBA, 4 );
	}
	VectorCopy( s->pos.trDelta, gent->client->ps.velocity );
	gent->client->ps.groundEntityNum = s->groundEntityNum;
	if ( cent->currentState.number != cg_localEntNum )
	{	// ours comes from the playerState (CG_CoopSyncLocalPlayer)
		gent->health = s->coopHealth;
		gent->max_health = s->coopMaxHealth & COOP_MAXHEALTH_MASK;
	}
	gent->client->renderInfo.lookMode = LM_ENT;
	gent->client->renderInfo.lookTarget = ( s->coopLookTarget >= 0 && s->coopLookTarget < ENTITYNUM_WORLD ) ? s->coopLookTarget : ENTITYNUM_NONE;
	gent->client->ps.eFlags = s->eFlags;
	gent->s.eFlags = s->eFlags;
	gent->client->ps.weapon = s->weapon;
	gent->client->ps.saberInFlight = s->saberInFlight;
	{	// Force visuals (s.coopForce, G_CoopUpdateAppearance); every check is "> cg.time"
		// and rewritten each frame, so cg.time + 1 stops the frame the host clears it
		playerState_t *ps = &gent->client->ps;
		if ( cent->currentState.number != cg_localEntNum )
		{	// ours already has forcePowersActive/forcePowerLevel/powerups/drain from the playerState
			ps->forcePowersActive = s->coopForce & COOPF_ACTIVE_MASK;
			ps->forcePowerLevel[FP_LIGHTNING] = ( s->coopForce >> COOPF_LVL_LIGHTNING_SHIFT ) & 3;
			ps->forcePowerLevel[FP_DRAIN] = ( s->coopForce >> COOPF_LVL_DRAIN_SHIFT ) & 3;
			ps->forcePowerLevel[FP_PROTECT] = ( s->coopForce >> COOPF_LVL_PROTECT_SHIFT ) & 3;
			ps->forcePowerLevel[FP_ABSORB] = ( s->coopForce >> COOPF_LVL_ABSORB_SHIFT ) & 3;
			ps->powerups[PW_FORCE_PUSH] = ( s->coopForce & COOPF_PUSH_LHAND ) ? cg.time + 1 : 0;
			ps->powerups[PW_FORCE_PUSH_RHAND] = ( s->coopForce & COOPF_PUSH_RHAND ) ? cg.time + 1 : 0;
			ps->forceDrainEntityNum = ( s->coopForce & COOPF_DRAIN_AREA ) ? ENTITYNUM_NONE : 0;
			ps->powerups[PW_SHOCKED] = s->coopShockTime;
		}
	}
	// riding: G_IsRidingVehicle reads s.m_iVehicleNum and the camera reads owner (g_vehicles.cpp Board)
	gent->s.m_iVehicleNum = s->m_iVehicleNum;
	if ( gent->client->NPC_class != CLASS_VEHICLE )
	{
		gent->owner = ( s->m_iVehicleNum > 0 && s->m_iVehicleNum < ENTITYNUM_WORLD ) ? &g_entities[s->m_iVehicleNum] : NULL;
	}
	else if ( gent->m_pVehicle )
	{
		gent->m_pVehicle->m_iArmor = s->coopHealth;
	}
	VectorCopy( cent->lerpOrigin, gent->currentOrigin );
	VectorCopy( cent->lerpAngles, gent->currentAngles );

	if ( st->legacyMd3 )
	{	// md3 legs/torso frames come from ps.legsAnim/torsoAnim (CG_PlayerAnimation); no ghoul2 to dress
		gent->client->ps.legsAnim = s->legsAnim;
		gent->client->ps.torsoAnim = s->torsoAnim;
		gent->client->ps.legsAnimTimer = s->legsAnimTimer;
		gent->client->ps.torsoAnimTimer = s->torsoAnimTimer;
		return;
	}

	CG_CoopSyncHandModel( gent, st );

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
				if ( blade->length <= 0.0f && b == 0 && step > 0.0f && saber->soundOn > 0 && saber->soundOn < MAX_SOUNDS )
				{	// ignition: CG_Player plays it when it grows the blade itself, but we grow it here first
					cgi_S_StartSound( NULL, s->number, CHAN_AUTO, cgs.sound_precache[saber->soundOn] );
				}
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
CG_CoopSyncGun

Emplaced gun / E-Web: CG_General keys its chair code on gent->s.weapon,
gent->activator (whose owner must be the gun and who must be EF_LOCKED_TO_WEAPON)
and gent->health; fill them from the wire. Turret: redo the host's pitch bone
override.
================
*/
static void CG_CoopSyncGun( centity_t *cent )
{
	gentity_t			*gent = cent->gent;
	const entityState_t	*s = &cent->currentState;
	const int			entNum = s->number;

	if ( coopGunKind[entNum] == COOP_GUN_TURRET )
	{
		if ( gent->playerModel < 0 || gent->playerModel >= gent->ghoul2.size() )
		{
			return;
		}
		if ( s->angles2[0] != coopTurretPitch[entNum][0] || s->angles2[1] != coopTurretPitch[entNum][1] )
		{
			const int	mode = (int)s->angles2[1];
			const float	pitch = s->angles2[0];
			vec3_t		angles;

			coopTurretPitch[entNum][0] = s->angles2[0];
			coopTurretPitch[entNum][1] = s->angles2[1];
			if ( mode & 2 )
			{	// turbo turret: "pitch" bone (turret_SetBoneAngles)
				VectorSet( angles, 0.0f, 0.0f, ( mode & 1 ) ? -pitch : pitch );
				gi.G2API_SetBoneAngles( &gent->ghoul2[gent->playerModel], "pitch", angles, BONE_ANGLES_POSTMULT, POSITIVE_Y, NEGATIVE_Z, NEGATIVE_X, NULL, 100, cg.time );
			}
			else
			{
				VectorSet( angles, ( mode & 1 ) ? pitch : -pitch, 0.0f, 0.0f );
				gi.G2API_SetBoneAngles( &gent->ghoul2[gent->playerModel], "Bone_body", angles, BONE_ANGLES_POSTMULT, POSITIVE_Y, POSITIVE_Z, POSITIVE_X, NULL, 100, cg.time );
			}
		}
		return;
	}

	gent->s.weapon = s->weapon;
	gent->s.apos = s->apos;		// the chair code copies s.apos.trBase into lerpAngles
	VectorCopy( s->angles, gent->s.angles );		// base yaw (E-Web turret offset)
	VectorCopy( cent->lerpOrigin, gent->currentOrigin );	// seat bolt matrix (CG_Player)
	gent->health = s->coopHealth;
	gent->max_health = Q_max( gent->max_health, gent->health );

	const int	userNum = s->otherEntityNum;
	gentity_t	*user = ( userNum >= 0 && userNum < ENTITYNUM_WORLD && g_entities[userNum].client ) ? &g_entities[userNum] : NULL;
	if ( gent->activator != user )
	{
		if ( gent->activator && gent->activator->owner == gent )
		{
			gent->activator->owner = NULL;
		}
		gent->activator = user;
		if ( cg_developer.integer )
		{
			Com_Printf( "coop: gun %i now used by %i\n", entNum, user ? userNum : -1 );
		}
	}
	if ( user )
	{
		user->owner = gent;
	}
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
	cent->gent->s.clientNum = cent->currentState.clientNum;	// lip sync / head bob index gi.VoiceVolume[] by it (CG_G2PlayerHeadAnims, CG_AddHeadBob)
	cent->gent->s.eType = cent->currentState.eType;	// the crosshair scan tests it (Force door hint)
	if ( cent->currentState.eType == ET_MISSILE )
	{	// alt-fire model/trail/loop sound (CG_Missile), relayed by SV_BuildClientSnapshot
		cent->gent->alt_fire = (qboolean)( cent->currentState.time2 != 0 );
		VectorCopy( cent->currentState.angles2, cent->gent->pos1 );
	}
	cent->gent->forcePushTime = cent->currentState.coopPushTime;	// push heat-haze; input lock while we are thrown
	if ( cent->currentState.eType == ET_MOVER && ( cent->currentState.coopHealth & ( COOP_MOVER_DOOR | COOP_MOVER_STATIC ) ) )
	{	// the crosshair scan reads classname/spawnflags (Force push/pull hint)
		cent->gent->classname = (char *)( ( cent->currentState.coopHealth & COOP_MOVER_DOOR ) ? "func_door" : "func_static" );
		cent->gent->spawnflags = cent->currentState.coopHealth & 0xff;
	}
	else if ( cent->currentState.eType == ET_MOVER )
	{	// not a Force mover (or a slot reused by one)
		cent->gent->classname = NULL;
		cent->gent->spawnflags = 0;
	}
	{	// the matrix (bullet time) camera entity: think it like the host does, once per instance
		const int num = cent->currentState.number;
		if ( cent->currentState.eType == ET_THINKER && cent->currentState.modelindex2 == COOP_THINKER_MATRIX )
		{
			if ( coopMatrixKey[num] != cent->currentState.time )
			{
				coopMatrixKey[num] = cent->currentState.time;
				cent->gent->e_clThinkFunc = clThinkF_CG_MatrixEffect;
			}
		}
		else if ( cent->gent->e_clThinkFunc == clThinkF_CG_MatrixEffect )
		{	// slot reused after the host freed it
			cent->gent->e_clThinkFunc = clThinkF_NULL;
			coopMatrixKey[num] = 0;
		}
	}
	CG_CoopEnsureG2Model( cent );
	CG_CoopEnsureCharacter( cent );
	CG_CoopSyncSoundSet( cent );
	if ( coopGunKind[cent->currentState.number] )
	{
		CG_CoopSyncGun( cent );
	}
	if ( cent->currentState.eType == ET_ITEM )
	{	// orientation / glow flags and saber pitch (G_CoopItemSyncFlags)
		cent->gent->spawnflags = cent->currentState.time2 & 0xff;
		cent->gent->random = (float)(short)( ( cent->currentState.time2 >> 8 ) & 0xffff );
	}
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
	{	// the joiner's side of G_CoopSaberWatch: inventory and saber definition as the host sends them
		static int seen = -1;
		const playerState_t *ps = &cg.snap->ps;
		const int now = ( ( ps->stats[STAT_WEAPONS] & ( 1 << WP_SABER ) ) ? 1 : 0 ) | ( ( ps->saber[0].name && ps->saber[0].name[0] ) ? 2 : 0 );

		if ( seen > 0 && ( seen & ~now ) && ps->stats[STAT_HEALTH] > 0 )
		{
			Com_Printf( "coop: SABRE PERDU (invite) : %s%s arme %i sabre '%s'\n", ( seen & ~now & 1 ) ? "[inventaire] " : "",
				( seen & ~now & 2 ) ? "[definition] " : "", ps->weapon, ps->saber[0].name ? ps->saber[0].name : "" );
		}
		seen = now;
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

	if ( cg.zoomMode == 1 || cg.zoomMode == 2 )
	{	// the host's pmove drives cg.zoomDir/zoomLocked from the buttons for slot 0 only (bg_pmove.cpp)
		usercmd_t cmd;
		if ( cgi_GetUserCmd( cgi_GetCurrentCmdNumber(), &cmd ) )
		{
			if ( cg.zoomMode == 1 )
			{
				if ( ( cmd.buttons & BUTTON_ALT_ATTACK ) && cg.snap->ps.batteryCharge )
				{
					cg.zoomLocked = qfalse;
					cg.zoomDir = 1;
				}
				else if ( ( cmd.buttons & BUTTON_ATTACK ) && cg.snap->ps.batteryCharge )
				{
					cg.zoomLocked = qfalse;
					cg.zoomDir = -1;
				}
				else
				{
					cg.zoomLocked = qtrue;
				}
			}
			else if ( !( cmd.buttons & BUTTON_ALT_ATTACK ) )
			{	// scope: releasing alt-fire locks the zoom where it is
				cg.zoomLocked = qtrue;
			}
		}
	}

	{	// the host frees the matrix entity 500 ms after ITS stop; if it left the snapshot before we
		// reached our own stop branch, drop the camera overrides rather than keep a stuck orbit
		extern int coopMatrixEnt;
		extern qboolean MatrixMode;
		if ( coopMatrixEnt >= 0 && !cg_entities[coopMatrixEnt].currentValid )
		{
			cg.overrides.active &= ~( CG_OVERRIDE_3RD_PERSON_RNG | CG_OVERRIDE_3RD_PERSON_ANG | CG_OVERRIDE_3RD_PERSON_POF );
			cg.overrides.thirdPersonAngle = cg.overrides.thirdPersonPitchOffset = cg.overrides.thirdPersonRange = 0;
			MatrixMode = qfalse;
			coopMatrixEnt = -1;
		}
	}


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
	float		bar;		// cinematic bar height
	float		barAlpha;
	vec4_t		fade;
	qboolean	cam;		// the camera is active (else the host only fades its screen)
	qboolean	cut;		// a jump from the previous sample: never interpolate into it
} coopCamSample_t;
static qboolean			coopCamPresent;		// the camera entity was in the last snapshot
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
	c->barAlpha = s->time2 / 255.0f;
	VectorCopy( s->angles2, c->fade );
	c->fade[3] = s->origin2[2];
	c->cam = (qboolean)( ( s->frame & 1 ) != 0 );
	c->cut = qfalse;
	if ( coopCamCount )
	{
		const coopCamSample_t *p = CG_CoopCamAt( coopCamCount - 1 );
		const int dt = time - p->time;
		if ( dt > 500 || p->cam != c->cam || Distance( p->origin, c->origin ) > 400.0f
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

/*
================
CG_CoopCheckPaksGen

The engine bumps cl_coopPaksGen when a pk3 from the host is added to the search
path (or unloaded). Every character we built is torn down; CG_CoopEnsureCharacter
rebuilds it from its spec at the next snapshot, this time with the real model
instead of the stormtrooper fallback (the renderer forgot its failed lookups).

The skin and model tables have to be registered again first. cgs.skins[] and
cgs.model_draw[] were filled when the level loaded (CG_RegisterGraphics) or as
each configstring arrived, i.e. while the host's pack was still missing, so
every entry naming it is 0. They are not just a cache: CG_AddPacketEntities
hands them to G2API_SetGhoul2ModelIndexes every frame, which sets each ghoul2's
render skin to cgs.skins[mCustomSkin] - so a character rebuilt with a perfectly
good skin is still drawn with no skin at all (the untextured grey character a
joiner sees after a download or a reconnect) until this table holds the handle.
================
*/
static void CG_CoopRegisterTablesAgain( void )
{
	int i, skins = 0, models = 0;

	for ( i = 1; i < MAX_CHARSKINS; i++ )
	{
		const char *name = CG_ConfigString( CS_CHARSKINS + i );
		if ( !name[0] )
		{
			continue;
		}
		const qhandle_t h = cgi_R_RegisterSkin( name );
		if ( h != cgs.skins[i] )
		{
			cgs.skins[i] = h;
			skins++;
		}
	}
	for ( i = 1; i < MAX_MODELS; i++ )
	{
		const char *name = CG_ConfigString( CS_MODELS + i );
		if ( !name[0] )
		{
			continue;
		}
		const qhandle_t h = cgi_R_RegisterModel( name );
		if ( h != cgs.model_draw[i] )
		{
			cgs.model_draw[i] = h;
			models++;
		}
	}
	if ( skins || models )
	{
		Com_Printf( "coop: %i skin(s) and %i model(s) registered again\n", skins, models );
	}
}

// called every frame on the host as well as on the joiners: the host loads a
// pack when a joiner finishes pushing its own mod up (sv_coop_transfer.cpp),
// and its cgs.skins[] / cgs.model_draw[] tables hold the 0 of the failed
// registration exactly like a joiner's do after a download. On the host
// coopChar[] is empty (the game module builds the characters and
// G_CoopCheckPaksGen builds them again), so only the tables are refreshed.
void CG_CoopCheckPaksGen( void )
{
	if ( coopPaksGenSeen == cg_coopPaksGen.integer )
	{
		return;
	}
	if ( coopPaksGenSeen != -1 )
	{
		int n = 0;
		// same as on the host: the .sab files of a pack that just arrived have
		// never been parsed, so re-read them before rebuilding the characters
		// (CG_CoopSetupSaber calls WP_SaberParseParms on that very list)
		WP_SaberLoadParms();
		CG_CoopRegisterTablesAgain();
		for ( int i = 0; i < MAX_GENTITIES; i++ )
		{
			if ( coopChar[i].specIndex && cg_entities[i].gent )
			{
				CG_CoopTearDown( &cg_entities[i] );
				n++;
			}
		}
		Com_Printf( "coop: pk3 list changed (gen %i), %i character(s) rebuilt\n", cg_coopPaksGen.integer, n );
	}
	coopPaksGenSeen = cg_coopPaksGen.integer;
}

// new level: whatever camera or fade the host had is gone with it
static void CG_CoopResetCamera( void )
{
	coopCameraActive = qfalse;
	coopCamPresent = qfalse;
	coopCamCount = coopCamHead = coopCamLastTime = 0;
	in_camera = false;
	memset( &client_camera, 0, sizeof( client_camera ) );
}

// the host left its camera: let the bars fade out as CGCam_Disable does
static void CG_CoopCameraEnd( void )
{
	coopCameraActive = qfalse;
	in_camera = false;
	client_camera.info_state = CAMERA_BAR_FADING;
	client_camera.bar_time = cg.time;
	client_camera.bar_alpha_source = client_camera.bar_alpha;
	client_camera.bar_alpha_dest = 0.0f;
	client_camera.bar_height_source = client_camera.bar_height;
	client_camera.bar_height_dest = 0.0f;
	client_camera.shake_duration = 0;
}

/*
================
CG_CoopSkip_f / CG_CoopTimescale_f

"skip N": the host toggled the cinematic skip. Our own skippingCinematic
makes the client freeze the screen on "SKIPPING", stop the sounds and drop
the captions (cl_main.cpp, cg_text.cpp), exactly as on the host.
"ts V": the script timescale; ours follows it below 1 (slow motion: our
clock then slows with the host's), never the 100 of a skip (the screen is
frozen anyway and a fast clock trips the net timeouts).
================
*/
// "credits": the campaign is over, roll the closing credits like the host
void CG_CoopCredits_f( void )
{
	if ( cg_remoteClient )
	{
		cgi_Cvar_Set( "cg_endcredits", "1" );
	}
}

void CG_CoopSkip_f( void )
{
	if ( cg_remoteClient )
	{
		cgi_Cvar_Set( "skippingCinematic", atoi( CG_Argv( 1 ) ) ? "1" : "0" );
	}
}

void CG_CoopTimescale_f( void )
{
	if ( cg_remoteClient )
	{
		float ts = atof( CG_Argv( 1 ) );
		if ( ts <= 0.0f || ts > 1.0f )
		{
			ts = 1.0f;
		}
		cgi_Cvar_Set( "timescale", va( "%g", ts ) );
	}
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
			CG_CoopCameraEnd();
		}
		if ( coopCamPresent )
		{	// the host's fade is over
			coopCamPresent = qfalse;
			client_camera.info_state &= ~CAMERA_FADING;
			client_camera.fade_color[3] = 0.0f;
		}
		coopCamCount = coopCamHead = coopCamLastTime = 0;
		return;
	}
	coopCamPresent = qtrue;

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

	// the host only fades its screen (no camera): drive the fade, keep our own view
	const qboolean camOn = ( t >= b->time ) ? b->cam : a->cam;
	if ( !camOn )
	{
		if ( coopCameraActive )
		{
			CG_CoopCameraEnd();
		}
		client_camera.info_state &= ~CAMERA_FADING;	// ours would fight it (CGCam_UpdateFade)
		for ( int i = 0; i < 4; i++ )
		{
			client_camera.fade_color[i] = a->fade[i] + f * ( b->fade[i] - a->fade[i] );
		}
		return;
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
	// the script FOV: CGCam_Update runs CG_CalcFOVFromX on it once, with our own aspect setting
	client_camera.FOV = a->fov + f * ( b->fov - a->fov );
	client_camera.FOV2 = client_camera.FOV;
	client_camera.bar_alpha = a->barAlpha + f * ( b->barAlpha - a->barAlpha );
	client_camera.bar_height = a->bar + f * ( b->bar - a->bar );
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
		const char *f[12];
		CG_CoopSplitSpec( spec, f, 12 );
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
		cent->currentState.coopForce = es->coopForce;
		cent->currentState.coopShockTime = es->coopShockTime;
		cent->currentState.coopPushTime = es->coopPushTime;
		cent->currentState.m_iVehicleNum = es->m_iVehicleNum;
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
// "wp <weapon>": the host put a weapon in our hand (saber handed by the host);
// our usercmd would switch straight back to the old selection otherwise
void CG_CoopSelectWeapon_f( void )
{
	const int wp = atoi( CG_Argv( 1 ) );
	if ( wp < WP_NONE || wp >= WP_NUM_WEAPONS )	// WP_NONE: riding a vehicle, dead on an emplaced gun
	{
		return;
	}
	if ( wp > WP_NONE && cg.snap && !( cg.snap->ps.stats[STAT_WEAPONS] & ( 1 << wp ) ) )
	{	// we do not own it: selecting it would leave the cgame pointing at a
		// weapon the host will never hand over, and the usercmd would keep
		// asking for it forever (CG_ChangeWeapon makes the same check)
		return;
	}
	cg.weaponSelect = wp;
	cg.weaponSelectTime = cg.time;
}

/*
================
CG_CoopZoom_f

"zoom <mode>": the host toggled our scope (disruptor alt-fire runs in its
pmove), or dropped our zoom (death, knockdown, view entity). Mirrors what
bg_pmove.cpp does to the host's own cg.zoomMode.
================
*/
extern float cg_zoomFov;
void CG_CoopZoom_f( void )
{
	const int mode = atoi( CG_Argv( 1 ) );
	if ( mode < 0 || mode > 3 || mode == cg.zoomMode )
	{
		return;
	}
	cg.zoomMode = mode;
	cg.zoomTime = cg.time;
	cg.zoomLocked = qfalse;
	if ( mode == 2 )
	{
		cg_zoomFov = 80.0f;
	}
}

/*
================
CG_CoopFog_f

"fog r g b": fx_rain lightning flash of the global fog (g_fx.cpp fx_rain_fog);
the host applies it straight to its renderer, we get the command.
================
*/
void CG_CoopFog_f( void )
{
	vec3_t color;

	if ( !cg_remoteClient || !gi.WE_SetTempGlobalFogColor )
	{
		return;
	}
	color[0] = atof( CG_Argv( 1 ) );
	color[1] = atof( CG_Argv( 2 ) );
	color[2] = atof( CG_Argv( 3 ) );
	if ( cg_developer.integer )
	{
		Com_Printf( "coop: fog flash %g %g %g\n", color[0], color[1], color[2] );
	}
	gi.WE_SetTempGlobalFogColor( color );
}

/*
================
Screen fades of a co-op death

A falling death (trigger_hurt FALLING, target_kill) fades the screen of the
player who fell. The host's game module writes the host's own client_camera
straight, and sends "fade" / "fadein" to a remote client (g_coop.cpp
G_CoopFadePlayer); CGCam_UpdateFade / CGCam_DrawWideScreen run outside
cutscenes here too, and CG_CoopSyncCamera only owns the fade while a
broadcast camera exists.
================
*/
// fade back from whatever is on the screen (no-op when nothing is up)
void CG_CoopFadeIn( int ms )
{
	vec4_t clear = { 0, 0, 0, 0 };
	if ( client_camera.fade_color[3] > 0.0f || ( client_camera.info_state & CAMERA_FADING ) )
	{
		CGCam_Fade( client_camera.fade_color, clear, ms );
	}
}

// "fade <ms> <sr> <sg> <sb> <sa> <dr> <dg> <db> <da>"
void CG_CoopFade_f( void )
{
	vec4_t src, dst;
	const int ms = atoi( CG_Argv( 1 ) );
	for ( int i = 0; i < 4; i++ )
	{
		src[i] = atof( CG_Argv( 2 + i ) );
		dst[i] = atof( CG_Argv( 6 + i ) );
	}
	CGCam_Fade( src, dst, ms );
}

// "fadein <ms>"
void CG_CoopFadeIn_f( void )
{
	CG_CoopFadeIn( atoi( CG_Argv( 1 ) ) );
}

// "tp <1|0|-1>": the host puts us in / out of third person (vehicle, emplaced
// gun); -1 = back to a gun, first person only if we want it (cg_gunAutoFirst)
void CG_CoopThirdPerson_f( void )
{
	const int mode = atoi( CG_Argv( 1 ) );
	if ( mode > 0 )
	{
		cgi_Cvar_Set( "cg_thirdperson", "1" );
	}
	else if ( mode == 0 || cg_gunAutoFirst.integer )
	{
		cgi_Cvar_Set( "cg_thirdperson", "0" );
	}
}

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
	if ( !Q_stricmp( CG_Argv( 1 ), "closeall" ) )
	{	// coop: the everyone-down screens go away (someone came back)
		cgi_UI_MenuCloseAll();
		coopAllDownShown = qfalse;
		return;
	}
	cgi_UI_SetActive_Menu( (char *)CG_Argv( 1 ) );
}

/*
==============================================================================
Downed teammates (host side: G_CoopTryDown & co in g_coop.cpp)

Same code for the host and a remote client: everything comes from the
snapshot. My own state is in ps.stats (STAT_COOP_DOWN ms left before
bleeding out, STAT_COOP_REVIVE progress, STAT_COOP_REVIVER the other one);
a teammate on the ground carries the PW_COOP_DOWNED bit in s.powerups.
"cad <seconds>" from the host drives the everyone-down screens.
==============================================================================
*/

extern qboolean CG_WorldCoordToScreenCoordFloat( vec3_t worldCoord, float *x, float *y );	// cg_draw.cpp

static qhandle_t	coopDownedIcon;
static char			coopReviveKey[32];
static int			coopReviveKeyTime;

// "cad <left>": seconds before the host reloads the checkpoint (-1: the host decides)
void CG_CoopAllDown_f( void )
{
	const int left = atoi( CG_Argv( 1 ) );
	coopAllDownShown = qtrue;
	if ( left < 0 )
	{
		cgi_Cvar_Set( "ui_coopAllDownText", G_CoopTr( "L'hote choisit comment continuer...", "The host is choosing how to continue..." ) );
	}
	else
	{
		cgi_Cvar_Set( "ui_coopAllDownText", va( G_CoopTr( "Retour au dernier point de controle dans %i s", "Back to the last checkpoint in %i s" ), left ) );
	}
}

// g_coopReviveRange as the host runs it (CVAR_SERVERINFO), default 80
static float CG_CoopReviveRange( void )
{
	const float r = atof( Info_ValueForKey( CG_ConfigString( CS_SERVERINFO ), "g_coopReviveRange" ) );
	return r > 0 ? r : 80.0f;
}

// name of the key bound to +coop_revive ("G"), refreshed now and then
static const char *CG_CoopReviveKeyName( void )
{
	if ( cg.time - coopReviveKeyTime > 1000 || !coopReviveKey[0] )
	{
		coopReviveKeyTime = cg.time;
		cgi_Key_BindingKeyName( "+coop_revive", coopReviveKey, sizeof( coopReviveKey ) );
		if ( !coopReviveKey[0] )
		{	// the use key revives too (G_CoopDownedThink)
			cgi_Key_BindingKeyName( "+use", coopReviveKey, sizeof( coopReviveKey ) );
		}
		if ( !coopReviveKey[0] )
		{
			Q_strncpyz( coopReviveKey, G_CoopTr( "(touche non liee)", "(key not bound)" ), sizeof( coopReviveKey ) );
		}
		else
		{
			Q_strupr( coopReviveKey );
		}
	}
	return coopReviveKey;
}

static void CG_CoopDrawBar( float x, float y, float w, float h, float frac, const vec4_t fill )
{
	static const vec4_t back = { 0, 0, 0, 0.6f };
	static const vec4_t edge = { 0.9f, 0.85f, 0.7f, 0.8f };
	if ( frac < 0 ) frac = 0;
	if ( frac > 1 ) frac = 1;
	CG_FillRect( x, y, w, h, back );
	CG_FillRect( x + 1, y + 1, ( w - 2 ) * frac, h - 2, fill );
	CG_DrawRect( x, y, w, h, 1, edge );
}

static void CG_CoopDrawCentered( float y, const char *text, const vec4_t color, int font, float scale )
{
	const int w = cgi_R_Font_StrLenPixels( text, font, scale );
	cgi_R_Font_DrawString( 320 - w / 2, y, text, color, font, -1, scale );
}

void CG_CoopDrawDowned( void )
{
	static const vec4_t red = { 1, 0.15f, 0.1f, 1 };
	static const vec4_t redFill = { 0.8f, 0.1f, 0.05f, 0.9f };
	static const vec4_t green = { 0.3f, 1, 0.3f, 1 };
	static const vec4_t greenFill = { 0.15f, 0.7f, 0.2f, 0.9f };
	static const vec4_t white = { 1, 1, 1, 1 };
	static const vec4_t dim = { 0.85f, 0.82f, 0.72f, 1 };

	if ( cg.snap && cg_remoteClient && coopHostVideo )
	{	// the world is held while the host watches its video (CG_CoopVideo_f)
		CG_CoopDrawCentered( 440, G_CoopTr( "L'hote regarde une video...", "The host is watching a video..." ), dim, cgs.media.qhFontSmall, 1.0f );
	}
	if ( !cg.snap || in_camera || cg.missionStatusShow || coopAllDownShown )
	{
		return;
	}
	const playerState_t *ps = &cg.snap->ps;
	const int	font = cgs.media.qhFontMedium;
	const int	small = cgs.media.qhFontSmall;
	float		nearest = -1, nearestBody = -1;

	if ( !coopDownedIcon )
	{
		coopDownedIcon = cgi_R_RegisterShaderNoMip( "gfx/jacoop/downed" );
	}

	// markers above teammates on the ground (screen positions: full-width 2D, not the anchored HUD grid)
	cgi_R_SetAspect2D( 0 );
	for ( int k = 0; k < MAX_CLIENTS; k++ )
	{
		const centity_t *cent = &cg_entities[k];
		if ( k == ps->clientNum || !cent->currentValid || !( cent->currentState.powerups & ( 1 << PW_COOP_DOWNED ) ) )
		{
			continue;
		}
		vec3_t	org;
		float	x, y;
		const float dist = Distance( cg.refdef.vieworg, cent->lerpOrigin );
		const float bodyDist = Distance( ps->origin, cent->lerpOrigin );
		if ( nearest < 0 || dist < nearest )
		{
			nearest = dist;
		}
		if ( nearestBody < 0 || bodyDist < nearestBody )
		{
			nearestBody = bodyDist;
		}
		VectorCopy( cent->lerpOrigin, org );
		org[2] += 40;
		if ( CG_WorldCoordToScreenCoordFloat( org, &x, &y ) )
		{
			const char *label = va( G_CoopTr( "A TERRE  %i m", "DOWN  %i m" ), (int)( dist / 32 ) );
			const int	w = cgi_R_Font_StrLenPixels( label, small, 0.9f );
			CG_DrawPic( x - 12, y - 28, 24, 24, coopDownedIcon );
			cgi_R_Font_DrawString( x - w / 2, y - 2, label, red, small, -1, 0.9f );
		}
	}
	cgi_R_SetAspect2D( 1 );

	if ( ps->stats[STAT_COOP_DOWN] > 0 )
	{	// I am on the ground
		const int	left = ps->stats[STAT_COOP_DOWN];
		if ( !coopDownMax )
		{
			Com_Printf( "coop: down, %i ms to bleed out\n", left );
		}
		if ( coopDownMax < left )
		{
			coopDownMax = left;
		}
		// red vignette
		for ( int i = 0; i < 4; i++ )
		{
			vec4_t shade = { 0.6f, 0, 0, 0.12f * ( 4 - i ) };
			const float t = i * 8.0f;
			CG_FillRect( 0, t, 640, 8, shade );
			CG_FillRect( 0, 472 - t, 640, 8, shade );
			CG_FillRect( t, 0, 8, 480, shade );
			CG_FillRect( 632 - t, 0, 8, 480, shade );
		}
		CG_CoopDrawCentered( 96, G_CoopTr( "A TERRE", "DOWN" ), red, font, 1.4f );
		if ( ps->stats[STAT_COOP_REVIVE] > 0 )
		{
			CG_CoopDrawCentered( 300, va( G_CoopTr( "Reanimation...  %i%%", "Reviving...  %i%%" ), ps->stats[STAT_COOP_REVIVE] ), green, font, 1.0f );
			CG_CoopDrawBar( 220, 326, 200, 12, ps->stats[STAT_COOP_REVIVE] / 100.0f, greenFill );
		}
		else
		{
			CG_CoopDrawCentered( 132, va( G_CoopTr( "Un coequipier peut te relever (touche %s pres de toi)", "A teammate can revive you (%s key next to you)" ), CG_CoopReviveKeyName() ), dim, small, 1.0f );
			CG_CoopDrawCentered( 300, va( G_CoopTr( "Saignement : %i s", "Bleeding out: %i s" ), ( left + 999 ) / 1000 ), white, font, 1.0f );
			CG_CoopDrawBar( 220, 326, 200, 12, coopDownMax > 0 ? (float)left / coopDownMax : 0, redFill );
		}
		return;
	}
	if ( coopDownMax )
	{
		Com_Printf( "coop: up again (%i hp)\n", ps->stats[STAT_HEALTH] );
		coopDownMax = 0;
	}

	if ( ps->stats[STAT_COOP_REVIVE] > 0 )
	{	// I am reviving someone
		CG_CoopDrawCentered( 300, va( G_CoopTr( "Vous relevez un coequipier...  %i%%", "Reviving a teammate...  %i%%" ), ps->stats[STAT_COOP_REVIVE] ), green, font, 1.0f );
		CG_CoopDrawBar( 220, 326, 200, 12, ps->stats[STAT_COOP_REVIVE] / 100.0f, greenFill );
	}
	else if ( nearestBody >= 0 && nearestBody <= CG_CoopReviveRange() && ps->stats[STAT_HEALTH] > 0 )
	{	// beside someone on the ground (same measure as the host: player origin to body, g_coopReviveRange)
		CG_CoopDrawCentered( 300, va( G_CoopTr( "Maintenir %s pour relever", "Hold %s to revive" ), CG_CoopReviveKeyName() ), white, font, 1.0f );
	}
}

/*
================
CG_CoopVideo_f

"vid <roq>" from the host: it plays an in-game video (SET_VIDEO_PLAY) and
holds the world meanwhile (SV_CoopHoldGame); play the same one here. "vid"
alone: the host's video is over. A joiner that skips its copy early sees the
held world with a caption until then.
================
*/
void CG_CoopVideo_f( void )
{
	const char *name = CG_Argv( 1 );

	if ( !cg_remoteClient )
	{
		return;
	}
	coopHostVideo = (qboolean)( name[0] != '\0' );
	if ( name[0] && strcmp( name, "-" ) )	// "-": the host is mid-video, caption only (SV_ClientEnterWorld)
	{
		cgi_SendConsoleCommand( va( "inGameCinematic %s\n", name ) );
	}
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
