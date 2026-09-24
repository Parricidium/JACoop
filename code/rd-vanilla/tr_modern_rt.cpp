/*
===========================================================================
JACoop - rendu moderne, le ray tracing (r_modernRT).

Le decor du BSP est range dans une hierarchie de boites (BVH) envoyee au GPU
une fois par carte ; les personnages, portes et autres sous-modeles, captures
au fil des surfaces dessinees, dans une seconde BVH refaite a chaque image.
Puis, pour chaque pixel, des rayons partent de sa position dans le monde :

  - vers le soleil, plusieurs, dans un petit cone : ombre douce, exacte, sans
    la carte d'ombre ni ses artefacts ;
  - dans l'hemisphere de la normale : occlusion vraie, plus limitee a l'ecran ;
  - vers chaque lumiere de la scene (lames de sabre avec leur couleur, tirs,
    explosions) : eclairage par pixel avec ombre portee ;
  - dans la direction du reflet, sur les surfaces brillantes : reflet exact
    (la couleur est reprise a l'ecran la ou le rayon atteint).

Tout est en GLSL 4.30 (tampons SSBO) : il faut une carte qui le supporte,
sinon la voie classique reprend sans rien dire d'autre qu'une ligne en console.
===========================================================================
*/
#include "tr_local.h"
#include "tr_modern.h"

#include <vector>
#include <algorithm>

cvar_t	*r_modernRT;
cvar_t	*r_modernRTSunRays;
cvar_t	*r_modernRTSoft;
cvar_t	*r_modernRTAORays;
cvar_t	*r_modernRTAORange;
cvar_t	*r_modernRTLights;
cvar_t	*r_modernRTLightScale;
cvar_t	*r_modernRTReflect;
cvar_t	*r_modernRTDynamic;
cvar_t	*r_modernRTScale;
cvar_t	*r_modernRTReflectStrength;
cvar_t	*r_modernRTNormals;
cvar_t	*r_modernRTNormalStrength;
cvar_t	*r_modernRTWater;
cvar_t	*r_modernRTGlass;
cvar_t	*r_modernRTLava;

extern bool g_bRenderGlowingObjects;

#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER	0x90D2
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW				0x88E4
#define GL_STREAM_DRAW				0x88E0
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER				0x8D40
#define GL_COLOR_ATTACHMENT0		0x8CE0
#define GL_DEPTH_ATTACHMENT			0x8D00
#define GL_FRAMEBUFFER_COMPLETE		0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24		0x81A6
#endif
#define RT_COLOR_ATTACHMENT( n )	( 0x8CE0 + ( n ) )

typedef void	(APIENTRY *PFN_glGenBuffers)( GLsizei, GLuint * );
typedef void	(APIENTRY *PFN_glDeleteBuffers)( GLsizei, const GLuint * );
typedef void	(APIENTRY *PFN_glBindBuffer)( GLenum, GLuint );
typedef void	(APIENTRY *PFN_glBufferData)( GLenum, ptrdiff_t, const void *, GLenum );
typedef void	(APIENTRY *PFN_glBindBufferBase)( GLenum, GLuint, GLuint );
typedef void	(APIENTRY *PFN_glDrawBuffers)( GLsizei, const GLenum * );
typedef void	(APIENTRY *PFN_glUniform4fv)( GLint, GLsizei, const GLfloat * );
typedef void	(APIENTRY *PFN_glUniform3fv)( GLint, GLsizei, const GLfloat * );

static PFN_glGenBuffers		p_glGenBuffers;
static PFN_glDeleteBuffers	p_glDeleteBuffers;
static PFN_glBindBuffer		p_glBindBuffer;
static PFN_glBufferData		p_glBufferData;
static PFN_glBindBufferBase	p_glBindBufferBase;
static PFN_glDrawBuffers	p_glDrawBuffers;
static PFN_glUniform4fv		p_glUniform4fv;
static PFN_glUniform3fv		p_glUniform3fv;

#define RT_MAX_LIGHTS		32
#define RT_MAX_DYN_TRIS		300000

// ---------------------------------------------------------------- l'etat
static qboolean	rtReady;			// points d'entree et programme en place
static qboolean	rtFailed;			// on a essaye, ca ne marchera pas : voie classique
static GLuint	rtProg;
static GLuint	rtFbo;				// trois cibles : occlusion/ombre, lumiere, reflet
static GLuint	rtLightTex, rtReflTex;
static GLuint	rtAoTex;			// notre cible occlusion/ombre, a l'echelle de la passe
static GLuint	rtAoTexBound;		// la cible 0 qu'on a attachee (celle de tr_modern)
static int		rtWidth, rtHeight;
static GLuint	rtMaskFbo, rtMaskTex, rtMaskDepth;	// le tampon de materiaux : normale, genre+brillance, profondeur
static GLuint	rtMatProg;						// le shader qui le remplit
static GLuint	rtLateProg;						// verre, eau, lave, apres le transparent
static GLuint	rtLateTex;						// l'image avec le transparent, pour les reflets tardifs
static qboolean	rtLateActive;					// la capture des surfaces tardives est ouverte
static int		rtLateFrame;
#define RT_MAX_LAVA	64
static vec3_t	rtLava[RT_MAX_LAVA];				// les nappes de lave de la carte (centres)
static float	rtLavaSize[RT_MAX_LAVA];
static int		rtLavaCount;
static GLuint	rtWorldNodes, rtWorldTris;			// SSBO du decor
static GLuint	rtDynNodes, rtDynTris;				// SSBO des sous-modeles et personnages
static int		rtWorldNodeCount, rtWorldTriCount;
static int		rtDynNodeCount;
static const world_t *rtWorldBuilt;		// pour quelle carte la BVH du decor a ete faite
static int		rtWorldSurfaces;
static char		rtWorldName[MAX_QPATH];
static qboolean	rtViewActive;			// la vue principale est en cours : capturer
static int		rtLastMod = -1;			// pour reagir a l'allumage (cg_shadows)

// ---------------------------------------------------------------- la BVH
struct rtTri {
	vec3_t	v[3];
	vec3_t	c;		// centre, pour le partage
};

struct rtNode {
	float	bmin[3];
	float	first;	// feuille : premier triangle ; noeud : enfant gauche (le droit suit)
	float	bmax[3];
	float	count;	// feuille : nombre de triangles ; noeud : 0
};

static std::vector<rtTri>	rtDyn;			// ce que les surfaces ont depose cette image
struct rtRange { int first, count; vec3_t bmin, bmax, c; };
static std::vector<rtRange>	rtDynEnts;		// ... groupe par entite (triangles contigus)
static const void			*rtDynLastEnt;
static std::vector<rtNode>	rtNodesTmp;
static std::vector<float>	rtTrisTmp;

static void RT_Bounds( const std::vector<rtTri> &t, int b, int e, vec3_t bmin, vec3_t bmax, vec3_t cmin, vec3_t cmax )
{
	VectorSet( bmin, 1e30f, 1e30f, 1e30f );
	VectorSet( bmax, -1e30f, -1e30f, -1e30f );
	VectorCopy( bmin, cmin );
	VectorCopy( bmax, cmax );
	for ( int i = b; i < e; i++ )
	{
		for ( int k = 0; k < 3; k++ )
		{
			for ( int a = 0; a < 3; a++ )
			{
				if ( t[i].v[k][a] < bmin[a] ) bmin[a] = t[i].v[k][a];
				if ( t[i].v[k][a] > bmax[a] ) bmax[a] = t[i].v[k][a];
			}
		}
		for ( int a = 0; a < 3; a++ )
		{
			if ( t[i].c[a] < cmin[a] ) cmin[a] = t[i].c[a];
			if ( t[i].c[a] > cmax[a] ) cmax[a] = t[i].c[a];
		}
	}
}

static float RT_Area( const vec3_t mn, const vec3_t mx )
{
	const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];

	return ( dx < 0.0f ) ? 0.0f : 2.0f * ( dx * dy + dy * dz + dz * dx );
}

// Construction par la surface (SAH) : le plan de coupe est celui, parmi 8 bacs
// sur chaque axe, qui minimise "aire gauche x triangles gauche + aire droite x
// triangles droite". Un rayon visite ainsi bien moins de boites qu'avec la
// mediane (mesure : la passe coutait le double).
#define RT_BINS	8

static void RT_BuildInto( std::vector<rtTri> &t, std::vector<rtNode> &n, int slot, int b, int e, int depth, qboolean sah )
{
	rtNode	nd;
	vec3_t	cmin, cmax;

	RT_Bounds( t, b, e, nd.bmin, nd.bmax, cmin, cmax );
	for ( int a = 0; a < 3; a++ )
	{	// un petit gras : les rayons rasants ne doivent pas passer entre deux boites
		nd.bmin[a] -= 0.05f;
		nd.bmax[a] += 0.05f;
	}
	const int count = e - b;

	if ( count <= ( sah ? 2 : 6 ) || depth > 48 )
	{
		nd.first = (float)b;
		nd.count = (float)count;
		n[slot] = nd;
		return;
	}

	// le meilleur plan
	int		bestAxis = -1, bestBin = -1;
	float	bestCost = (float)count;		// le cout de la feuille

	for ( int axis = 0; axis < ( sah ? 3 : 0 ); axis++ )
	{
		const float extent = cmax[axis] - cmin[axis];

		if ( extent <= 1e-4f )
		{
			continue;
		}
		int		cnt[RT_BINS] = { 0 };
		vec3_t	bmn[RT_BINS], bmx[RT_BINS];

		for ( int k = 0; k < RT_BINS; k++ )
		{
			VectorSet( bmn[k], 1e30f, 1e30f, 1e30f );
			VectorSet( bmx[k], -1e30f, -1e30f, -1e30f );
		}
		const float scale = (float)RT_BINS / extent;

		for ( int i = b; i < e; i++ )
		{
			int k = (int)( ( t[i].c[axis] - cmin[axis] ) * scale );

			if ( k < 0 ) k = 0;
			if ( k >= RT_BINS ) k = RT_BINS - 1;
			cnt[k]++;
			for ( int v = 0; v < 3; v++ )
			{
				for ( int a = 0; a < 3; a++ )
				{
					if ( t[i].v[v][a] < bmn[k][a] ) bmn[k][a] = t[i].v[v][a];
					if ( t[i].v[v][a] > bmx[k][a] ) bmx[k][a] = t[i].v[v][a];
				}
			}
		}
		// balayage : aires cumulees de droite, puis de gauche
		float	rightArea[RT_BINS];
		int		rightCnt[RT_BINS];
		vec3_t	amn, amx;

		VectorSet( amn, 1e30f, 1e30f, 1e30f );
		VectorSet( amx, -1e30f, -1e30f, -1e30f );
		int rc = 0;
		for ( int k = RT_BINS - 1; k > 0; k-- )
		{
			for ( int a = 0; a < 3; a++ )
			{
				if ( bmn[k][a] < amn[a] ) amn[a] = bmn[k][a];
				if ( bmx[k][a] > amx[a] ) amx[a] = bmx[k][a];
			}
			rc += cnt[k];
			rightArea[k] = RT_Area( amn, amx );
			rightCnt[k] = rc;
		}
		VectorSet( amn, 1e30f, 1e30f, 1e30f );
		VectorSet( amx, -1e30f, -1e30f, -1e30f );
		int lc = 0;
		const float parentArea = RT_Area( nd.bmin, nd.bmax );

		for ( int k = 0; k < RT_BINS - 1; k++ )
		{
			for ( int a = 0; a < 3; a++ )
			{
				if ( bmn[k][a] < amn[a] ) amn[a] = bmn[k][a];
				if ( bmx[k][a] > amx[a] ) amx[a] = bmx[k][a];
			}
			lc += cnt[k];
			if ( lc == 0 || rightCnt[k + 1] == 0 || parentArea <= 0.0f )
			{
				continue;
			}
			const float cost = 0.5f + ( RT_Area( amn, amx ) * lc + rightArea[k + 1] * rightCnt[k + 1] ) / parentArea;

			if ( cost < bestCost )
			{
				bestCost = cost;
				bestAxis = axis;
				bestBin = k;
			}
		}
	}

	int mid;

	if ( bestAxis < 0 )
	{	// aucune coupe ne vaut mieux qu'une feuille (ou pas de SAH demande)
		if ( sah && count <= 8 )
		{
			nd.first = (float)b;
			nd.count = (float)count;
			n[slot] = nd;
			return;
		}
		// trop gros pour une feuille : la mediane sur l'axe le plus long
		int		axis = 0;
		float	best = cmax[0] - cmin[0];

		for ( int a = 1; a < 3; a++ )
		{
			if ( cmax[a] - cmin[a] > best )
			{
				best = cmax[a] - cmin[a];
				axis = a;
			}
		}
		mid = ( b + e ) / 2;
		std::nth_element( t.begin() + b, t.begin() + mid, t.begin() + e,
			[axis]( const rtTri &x, const rtTri &y ) { return x.c[axis] < y.c[axis]; } );
	}
	else
	{
		const float	plane = cmin[bestAxis] + ( bestBin + 1 ) * ( cmax[bestAxis] - cmin[bestAxis] ) / (float)RT_BINS;
		const int	axis = bestAxis;

		mid = (int)( std::partition( t.begin() + b, t.begin() + e,
			[axis, plane]( const rtTri &x ) { return x.c[axis] < plane; } ) - t.begin() );
		if ( mid == b || mid == e )
		{
			mid = ( b + e ) / 2;
		}
	}

	const int left = (int)n.size();

	n.push_back( rtNode() );
	n.push_back( rtNode() );
	nd.first = (float)left;
	nd.count = 0.0f;
	n[slot] = nd;
	RT_BuildInto( t, n, left, b, mid, depth + 1, sah );
	RT_BuildInto( t, n, left + 1, mid, e, depth + 1, sah );
}

// Construit la BVH de t (reordonne t), et remplit les tampons plats : 8 flottants
// par noeud, 12 par triangle (sommet, deux aretes - la forme qu'aime le test
// de Moller-Trumbore).
// Le niveau du dessus de l'arbre dynamique : une boite par entite, coupee a
// la mediane des centres ; sous chaque entite, sa propre BVH (ses triangles
// sont contigus). Un rayon qui ne passe pres d'aucun perso est rejete a la
// racine ou juste dessous, au lieu de descendre dans un arbre qui melange
// tous les persos de la carte.
static void RT_BuildEntities( std::vector<rtTri> &t, std::vector<rtNode> &n, std::vector<rtRange> &ents, int slot, int b, int e, int depth )
{
	if ( e - b == 1 )
	{
		RT_BuildInto( t, n, slot, ents[b].first, ents[b].first + ents[b].count, depth, qfalse );
		return;
	}
	rtNode	nd;

	VectorSet( nd.bmin, 1e30f, 1e30f, 1e30f );
	VectorSet( nd.bmax, -1e30f, -1e30f, -1e30f );
	for ( int i = b; i < e; i++ )
	{
		for ( int a = 0; a < 3; a++ )
		{
			if ( ents[i].bmin[a] < nd.bmin[a] ) nd.bmin[a] = ents[i].bmin[a];
			if ( ents[i].bmax[a] > nd.bmax[a] ) nd.bmax[a] = ents[i].bmax[a];
		}
	}
	int		axis = 0;
	float	best = nd.bmax[0] - nd.bmin[0];

	for ( int a = 1; a < 3; a++ )
	{
		if ( nd.bmax[a] - nd.bmin[a] > best )
		{
			best = nd.bmax[a] - nd.bmin[a];
			axis = a;
		}
	}
	const int mid = ( b + e ) / 2;

	std::nth_element( ents.begin() + b, ents.begin() + mid, ents.begin() + e,
		[axis]( const rtRange &x, const rtRange &y ) { return x.c[axis] < y.c[axis]; } );

	const int left = (int)n.size();

	n.push_back( rtNode() );
	n.push_back( rtNode() );
	nd.first = (float)left;
	nd.count = 0.0f;
	n[slot] = nd;
	RT_BuildEntities( t, n, ents, left, b, mid, depth + 1 );
	RT_BuildEntities( t, n, ents, left + 1, mid, e, depth + 1 );
}

static void RT_Build( std::vector<rtTri> &t, std::vector<rtNode> &nodes, std::vector<float> &tris, std::vector<rtRange> *ents = NULL )
{
	nodes.clear();
	tris.clear();
	if ( t.empty() )
	{
		return;
	}
	nodes.reserve( t.size() );
	nodes.push_back( rtNode() );
	if ( ents && ents->size() > 1 )
	{
		for ( size_t i = 0; i < ents->size(); i++ )
		{
			vec3_t cmn, cmx;

			RT_Bounds( t, (*ents)[i].first, (*ents)[i].first + (*ents)[i].count, (*ents)[i].bmin, (*ents)[i].bmax, cmn, cmx );
			VectorAdd( (*ents)[i].bmin, (*ents)[i].bmax, (*ents)[i].c );
			VectorScale( (*ents)[i].c, 0.5f, (*ents)[i].c );
		}
		RT_BuildEntities( t, nodes, *ents, 0, 0, (int)ents->size(), 0 );
	}
	else
	{
		RT_BuildInto( t, nodes, 0, 0, (int)t.size(), 0, ents ? qfalse : qtrue );
	}

	tris.resize( t.size() * 12 );
	for ( size_t i = 0; i < t.size(); i++ )
	{
		float *o = &tris[i * 12];

		VectorCopy( t[i].v[0], o );
		o[3] = 0.0f;
		VectorSubtract( t[i].v[1], t[i].v[0], o + 4 );
		o[7] = 0.0f;
		VectorSubtract( t[i].v[2], t[i].v[0], o + 8 );
		o[11] = 0.0f;
	}
}

static void RT_PushTri( std::vector<rtTri> &out, const float *a, const float *b, const float *c )
{
	rtTri	t;

	VectorCopy( a, t.v[0] );
	VectorCopy( b, t.v[1] );
	VectorCopy( c, t.v[2] );
	VectorAdd( t.v[0], t.v[1], t.c );
	VectorAdd( t.c, t.v[2], t.c );
	VectorScale( t.c, 1.0f / 3.0f, t.c );
	out.push_back( t );
}

// Le genre d'une surface pour le tampon de materiaux : 0 opaque, 1 verre,
// 2 eau, 3 lave, -1 rien a faire.
static int RT_ShaderKind( const shader_t *sh )
{
	if ( !sh || ( sh->surfaceFlags & ( SURF_NODRAW | SURF_SKY ) ) || !sh->stages || sh->numUnfoggedPasses < 1 )
	{
		return -1;
	}
	if ( sh->contentFlags & CONTENTS_LAVA )
	{
		return r_modernRTLava->integer ? 3 : -1;
	}
	if ( sh->contentFlags & CONTENTS_WATER )
	{
		return r_modernRTWater->integer ? 2 : -1;
	}
	if ( sh->sort > SS_OPAQUE )
	{	// transparent : seulement le verre (une lueur d'environnement, ou son nom)
		if ( !r_modernRTGlass->integer )
		{
			return -1;
		}
		if ( Q_stristr( sh->name, "glass" ) || Q_stristr( sh->name, "window" ) || Q_stristr( sh->name, "vitre" ) )
		{
			return 1;
		}
		for ( int i = 0; i < sh->numUnfoggedPasses; i++ )
		{
			if ( sh->stages[i].bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED )
			{
				return 1;
			}
		}
		return -1;
	}
	return 0;
}

// La texture de base d'un shader (le premier etage qui n'est pas la carte de
// lumiere), pour en tirer le relief.
static const image_t *RT_ShaderDiffuse( const shader_t *sh )
{
	for ( int i = 0; i < sh->numUnfoggedPasses; i++ )
	{
		const textureBundle_t *b = &sh->stages[i].bundle[0];

		if ( b->image && !b->isLightmap && !b->isVideoMap && b->image->width > 1 )
		{
			return b->image;
		}
	}
	return NULL;
}

static qboolean RT_ShaderCasts( const shader_t *sh )
{
	if ( !sh || sh->sort > SS_OPAQUE || ( sh->surfaceFlags & ( SURF_NODRAW | SURF_SKY ) ) )
	{
		return qfalse;		// ni le ciel, ni le transparent
	}
	if ( sh->numUnfoggedPasses > 0 && sh->stages && ( sh->stages[0].stateBits & GLS_ATEST_BITS ) )
	{
		return qfalse;		// feuillages decoupes : un carre plein ferait une ombre de panneau
	}
	return qtrue;
}

// Le decor : les surfaces du monde (bmodels[0] - les portes et autres sous-
// modeles bougent, ils passent par la capture dynamique).
static void RT_GatherWorld( std::vector<rtTri> &out )
{
	const bmodel_t	*world = &tr.world->bmodels[0];

	for ( int i = 0; i < world->numSurfaces; i++ )
	{
		const msurface_t *surf = &world->firstSurface[i];

		if ( !surf->data || !RT_ShaderCasts( surf->shader ) )
		{
			continue;
		}
		if ( *surf->data == SF_FACE )
		{
			const srfSurfaceFace_t	*face = (const srfSurfaceFace_t *)surf->data;
			const unsigned int		*idx = (const unsigned int *)( ( (const byte *)face ) + face->ofsIndices );

			for ( int k = 0; k + 2 < face->numIndices; k += 3 )
			{
				RT_PushTri( out, face->points[idx[k]], face->points[idx[k + 1]], face->points[idx[k + 2]] );
			}
		}
		else if ( *surf->data == SF_TRIANGLES )
		{
			const srfTriangles_t *tri = (const srfTriangles_t *)surf->data;

			for ( int k = 0; k + 2 < tri->numIndexes; k += 3 )
			{
				RT_PushTri( out, tri->verts[tri->indexes[k]].xyz, tri->verts[tri->indexes[k + 1]].xyz, tri->verts[tri->indexes[k + 2]].xyz );
			}
		}
		else if ( *surf->data == SF_GRID )
		{
			const srfGridMesh_t	*grid = (const srfGridMesh_t *)surf->data;
			const int			w = grid->width, h = grid->height;

			for ( int r = 0; r + 1 < h; r++ )
			{
				for ( int c = 0; c + 1 < w; c++ )
				{
					const int a = r * w + c;

					RT_PushTri( out, grid->verts[a].xyz, grid->verts[a + 1].xyz, grid->verts[a + w].xyz );
					RT_PushTri( out, grid->verts[a + 1].xyz, grid->verts[a + w + 1].xyz, grid->verts[a + w].xyz );
				}
			}
		}
	}
}

static void RT_Upload( GLuint buf, const void *data, size_t bytes, GLenum usage )
{
	p_glBindBuffer( GL_SHADER_STORAGE_BUFFER, buf );
	p_glBufferData( GL_SHADER_STORAGE_BUFFER, (ptrdiff_t)( bytes ? bytes : 16 ), bytes ? data : NULL, usage );
	p_glBindBuffer( GL_SHADER_STORAGE_BUFFER, 0 );
}

// ---------------------------------------------------------------- les shaders
static const char *rtVertex =
	"#version 430 compatibility\n"
	"out vec2 uv;\n"
	"void main() {\n"
	"	uv = gl_MultiTexCoord0.xy;\n"
	"	gl_Position = gl_Vertex;\n"
	"}\n";

// Le parcours de la BVH est une macro : GLSL n'accepte pas un tampon en
// parametre, il faut une fonction par tampon (decor, dynamique).
static const char *rtFragment = R"GLSL(#version 430 compatibility
uniform sampler2D sceneDepth;
uniform sampler2D scene;
uniform sampler2D shineMask;
uniform sampler2D shineDepth;
uniform vec2 projAB;
uniform vec2 projCD;
uniform vec2 projEF;
uniform vec2 texel;
uniform mat4 invView;
uniform mat4 viewMat;
uniform vec3 sunDir;
uniform float sunOn;
uniform float sunSoft;
uniform int sunRays;
uniform float aoOn;
uniform float aoRange;
uniform int aoRays;
uniform int numLights;
uniform vec4 lPos[32];
uniform vec4 lCol[32];
uniform vec4 lEnd[32];
uniform float lightScale;
uniform float reflOn;
uniform float reflStrength;
uniform int dynNodes;
layout(std430, binding = 0) readonly buffer B0 { vec4 wn[]; };
layout(std430, binding = 1) readonly buffer B1 { vec4 wt[]; };
layout(std430, binding = 2) readonly buffer B2 { vec4 dn[]; };
layout(std430, binding = 3) readonly buffer B3 { vec4 dt[]; };
in vec2 uv;
layout(location = 0) out vec4 outAo;
layout(location = 1) out vec4 outLight;
layout(location = 2) out vec4 outRefl;

vec3 viewPosAt( vec2 t, float d ) {
	vec3 n = vec3( t * 2.0 - 1.0, d * 2.0 - 1.0 );
	float w = n.z / projEF.y + projEF.x / projEF.y;
	vec3 p = vec3( ( n.x + projCD.x ) / projAB.x, ( n.y + projCD.y ) / projAB.y, -1.0 );
	return p / w;
}
vec3 viewPos( vec2 t ) { return viewPosAt( t, texture( sceneDepth, t ).r ); }
vec3 normalAt( vec2 t, vec3 p ) {
	vec3 l = viewPos( t - vec2( texel.x, 0.0 ) ) - p;
	vec3 r = viewPos( t + vec2( texel.x, 0.0 ) ) - p;
	vec3 d = viewPos( t - vec2( 0.0, texel.y ) ) - p;
	vec3 u = viewPos( t + vec2( 0.0, texel.y ) ) - p;
	vec3 h = ( abs( l.z ) < abs( r.z ) ) ? -l : r;
	vec3 v = ( abs( d.z ) < abs( u.z ) ) ? -d : u;
	return normalize( cross( h, v ) );
}
float hash( vec2 c ) { return fract( sin( dot( c, vec2( 12.9898, 78.233 ) ) ) * 43758.5453 ); }

#define TRAVERSE( NAME, NODES, TRIS ) \
float NAME( vec3 ro, vec3 rd, float tmax, bool any ) { \
	vec3 inv = 1.0 / ( abs( rd ) + vec3( 1e-9 ) ) * sign( rd + vec3( 1e-12 ) ); \
	int stack[48]; int sp = 0; float best = tmax; \
	{ vec4 a = NODES[0]; vec4 b = NODES[1]; \
	  vec3 t0 = ( a.xyz - ro ) * inv; vec3 t1 = ( b.xyz - ro ) * inv; vec3 tmn = min( t0, t1 ); vec3 tmx = max( t0, t1 ); \
	  if ( max( max( tmn.x, tmn.y ), max( tmn.z, 0.0 ) ) > min( min( tmx.x, tmx.y ), min( tmx.z, best ) ) ) return best; } \
	int node = 0; \
	for ( int it = 0; it < 4096; it++ ) { \
		vec4 a = NODES[node * 2]; vec4 b = NODES[node * 2 + 1]; \
		int first = int( a.w ); int cnt = int( b.w ); \
		if ( cnt > 0 ) { \
			for ( int i = 0; i < cnt; i++ ) { \
				int t = ( first + i ) * 3; \
				vec3 v0 = TRIS[t].xyz; vec3 e1 = TRIS[t + 1].xyz; vec3 e2 = TRIS[t + 2].xyz; \
				vec3 pv = cross( rd, e2 ); float det = dot( e1, pv ); \
				if ( abs( det ) < 1e-7 ) continue; \
				float id = 1.0 / det; vec3 s = ro - v0; float u = dot( s, pv ) * id; \
				if ( u < 0.0 || u > 1.0 ) continue; \
				vec3 q = cross( s, e1 ); float v = dot( rd, q ) * id; \
				if ( v < 0.0 || u + v > 1.0 ) continue; \
				float tt = dot( e2, q ) * id; \
				if ( tt > 0.02 && tt < best ) { best = tt; if ( any ) return best; } \
			} \
			if ( sp == 0 ) break; \
			node = stack[--sp]; continue; \
		} \
		vec4 la = NODES[first * 2]; vec4 lb = NODES[first * 2 + 1]; \
		vec4 ra = NODES[first * 2 + 2]; vec4 rb = NODES[first * 2 + 3]; \
		vec3 lt0 = ( la.xyz - ro ) * inv; vec3 lt1 = ( lb.xyz - ro ) * inv; vec3 ln = min( lt0, lt1 ); vec3 lx = max( lt0, lt1 ); \
		float ltn = max( max( ln.x, ln.y ), max( ln.z, 0.0 ) ); float ltf = min( min( lx.x, lx.y ), min( lx.z, best ) ); \
		vec3 rt0 = ( ra.xyz - ro ) * inv; vec3 rt1 = ( rb.xyz - ro ) * inv; vec3 rn = min( rt0, rt1 ); vec3 rx = max( rt0, rt1 ); \
		float rtn = max( max( rn.x, rn.y ), max( rn.z, 0.0 ) ); float rtf = min( min( rx.x, rx.y ), min( rx.z, best ) ); \
		bool hl = ltn <= ltf; bool hr = rtn <= rtf; \
		if ( hl && hr ) { \
			if ( ltn <= rtn ) { node = first; if ( sp < 48 ) stack[sp++] = first + 1; } \
			else { node = first + 1; if ( sp < 48 ) stack[sp++] = first; } \
		} else if ( hl ) { node = first; } \
		else if ( hr ) { node = first + 1; } \
		else { if ( sp == 0 ) break; node = stack[--sp]; } \
	} \
	return best; \
}
TRAVERSE( travW, wn, wt )
TRAVERSE( travD, dn, dt )

// dynMax : jusqu'ou les persos comptent (l'occlusion ne leur demande qu'une
// ombre de contact : 64 unites, le reste serait paye pour rien)
float trace( vec3 ro, vec3 rd, float tmax, bool any, float dynMax ) {
	float t = travW( ro, rd, tmax, any );
	if ( any && t < tmax ) return t;
	if ( dynNodes > 0 ) {
		// ne garder le resultat dynamique que s'il touche vraiment : travD rend
		// sa borne quand il ne touche rien, et la borne min( t, dynMax ) faisait
		// croire a l'occlusion que chaque rayon butait a 64 unites (mesure :
		// occlusion moyenne 86/255 au lieu de 241)
		float lim = min( t, dynMax );
		float td = travD( ro, rd, lim, any );
		if ( td < lim ) t = td;
	}
	return t;
}

void main() {
	float d = texture( sceneDepth, uv ).r;
	if ( d >= 1.0 ) { outAo = vec4( 1.0 ); outLight = vec4( 0.0 ); outRefl = vec4( 0.0 ); return; }
	vec3 pv = viewPosAt( uv, d );
	vec3 nv = normalAt( uv, pv );
	vec3 p = ( invView * vec4( pv, 1.0 ) ).xyz;
	vec3 n = normalize( mat3( invView ) * nv );
	vec3 camPos = invView[3].xyz;
	vec3 o = p + n * 0.75;
	// la normale de relief du decor (tampon de materiaux), pour l'eclairage et
	// les reflets ; les rayons d'ombre gardent la normale geometrique
	vec4 mm = texture( shineMask, uv );
	float mdd = texture( shineDepth, uv ).r;
	bool hasMat = ( mm.a > 0.0 && abs( mdd - d ) < 0.0004 );
	vec3 nd = hasMat ? normalize( mat3( invView ) * normalize( mm.xyz * 2.0 - 1.0 ) ) : n;
	vec3 t1 = normalize( cross( n, ( abs( n.y ) < 0.9 ) ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 ) ) );
	vec3 t2 = cross( n, t1 );

	// --- le soleil : plusieurs rayons dans un cone, pour la penombre
	float shade = 1.0;
	if ( sunOn > 0.5 ) {
		float ndl = dot( n, sunDir );
		if ( ndl > 0.02 ) {
			vec3 s1 = normalize( cross( sunDir, ( abs( sunDir.y ) < 0.9 ) ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 ) ) );
			vec3 s2 = cross( sunDir, s1 );
			// adaptatif : deux rayons opposes dans le cone ; s'ils sont d'accord
			// (plein soleil ou pleine ombre, l'immense majorite des pixels), on
			// s'arrete la ; sinon on est dans la penombre et on tire le reste
			float lit = 0.0; int done = 0;
			for ( int i = 0; i < sunRays; i++ ) {
				float a = 6.2831853 * hash( gl_FragCoord.xy + vec2( float( i ) * 17.0, 3.0 ) ) + ( ( i == 1 ) ? 3.1415926 : 0.0 );
				float r = sqrt( hash( gl_FragCoord.yx + vec2( 1.0, float( i ) * 5.0 ) ) ) * sunSoft;
				vec3 dir = normalize( sunDir + ( s1 * cos( a ) + s2 * sin( a ) ) * r );
				lit += ( trace( o + sunDir * 0.25, dir, 16384.0, true, 16384.0 ) < 16384.0 ) ? 0.0 : 1.0;
				done++;
				if ( i == 1 && ( lit < 0.5 || lit > 1.5 ) ) break;
			}
			shade = lit / float( done );
		}
	}

	// --- l'occlusion : l'hemisphere de la normale, ponderee par la distance
	float ao = 1.0;
	if ( aoOn > 0.5 ) {
		float occ = 0.0;
		for ( int i = 0; i < aoRays; i++ ) {
			float u1 = hash( gl_FragCoord.xy * 1.3 + vec2( float( i ) * 7.1, 0.0 ) );
			float u2 = hash( gl_FragCoord.yx * 0.7 + vec2( 9.0, float( i ) * 3.3 ) );
			float r = sqrt( u1 ); float a = 6.2831853 * u2;
			vec3 dir = t1 * ( r * cos( a ) ) + t2 * ( r * sin( a ) ) + n * sqrt( max( 0.0, 1.0 - u1 ) );
			float t = trace( o, dir, aoRange, false, 64.0 );
			if ( t < aoRange ) occ += 1.0 - t / aoRange;
		}
		ao = 1.0 - occ / float( aoRays );
	}

	// --- les lumieres : par pixel, avec leur ombre
	vec3 light = vec3( 0.0 );
	for ( int i = 0; i < numLights; i++ ) {
		vec3 lp = lPos[i].xyz; float rad = lPos[i].w;
		if ( lCol[i].w > 0.5 && lCol[i].w < 1.5 ) {	// une lame : le point du segment le plus proche
			vec3 ab = lEnd[i].xyz - lp; float len2 = dot( ab, ab );
			float s = ( len2 > 0.0 ) ? clamp( dot( p - lp, ab ) / len2, 0.0, 1.0 ) : 0.0;
			lp += ab * s;
		}
		vec3 L = lp - p; float dist = length( L );
		if ( dist >= rad || dist < 0.5 ) continue;
		L /= dist;
		float ndl = dot( nd, L );
		if ( ndl <= 0.0 || dot( n, L ) <= -0.2 ) continue;
		float att = 1.0 - dist / rad; att *= att;
		float vis = ( trace( o, L, dist - 1.0, true, dist ) < dist - 1.0 ) ? 0.0 : 1.0;
		light += lCol[i].rgb * ( ndl * att * vis );
	}
	light *= lightScale;

	// --- le reflet, la ou la surface est brillante
	vec4 refl = vec4( 0.0 );
	if ( reflOn > 0.5 ) {
		float m = fract( mm.a * 5.0 );
		if ( hasMat && abs( floor( mm.a * 5.0 + 0.001 ) - 1.0 ) < 0.5 && m > 0.01 ) {
			vec3 vdir = normalize( p - camPos );
			vec3 rd = reflect( vdir, nd );
			float t = trace( o, rd, 8192.0, false, 8192.0 );
			if ( t < 8192.0 ) {
				vec4 hv = viewMat * vec4( p + rd * t, 1.0 );
				if ( hv.z < -1.0 ) {
					vec2 st = vec2( hv.x / -hv.z * projAB.x - projCD.x, hv.y / -hv.z * projAB.y - projCD.y ) * 0.5 + 0.5;
					if ( st.x > 0.0 && st.x < 1.0 && st.y > 0.0 && st.y < 1.0 ) {
						float sz = viewPos( st ).z;
						if ( abs( sz - hv.z ) < 12.0 + 0.02 * -hv.z ) {
							float fres = 0.35 + 0.65 * pow( 1.0 - max( dot( -vdir, nd ), 0.0 ), 3.0 );
							refl = vec4( texture( scene, st ).rgb, clamp( m * fres * reflStrength, 0.0, 0.9 ) );
						}
					}
				}
			}
		}
	}

	outAo = vec4( ao, shade, 0.0, 1.0 );
	outLight = vec4( light, 0.0 );
	outRefl = refl;
}
)GLSL";

// Le tampon de materiaux. La normale de relief vient de la texture elle-meme
// (le gradient de sa luminance, comme les normal maps generees d'Arx), posee
// dans un repere tangent construit par les derivees d'ecran - le BSP n'a pas
// de tangentes. L'eau et la lave ont des vagues procedurales dans ce repere.
static const char *rtMatVertex =
	"#version 430 compatibility\n"
	"out vec3 vp;\n"
	"out vec2 uv;\n"
	"void main() {\n"
	"	vp = ( gl_ModelViewMatrix * gl_Vertex ).xyz;\n"
	"	uv = gl_MultiTexCoord0.xy;\n"
	"	gl_Position = ftransform();\n"
	"}\n";

static const char *rtMatFragment = R"GLSL(#version 430 compatibility
uniform sampler2D diffuse;
uniform vec2 texel;
uniform float strength;
uniform float kind;		// 0 opaque, 1 verre, 2 eau, 3 lave
uniform float shine;
uniform float time;
uniform mat4 invView;
in vec3 vp;
in vec2 uv;
out vec4 outMat;
float lum( vec2 t ) { vec3 c = texture( diffuse, t ).rgb; return dot( c, vec3( 0.3, 0.59, 0.11 ) ); }
void main() {
	vec3 dp1 = dFdx( vp ), dp2 = dFdy( vp );
	vec2 duv1 = dFdx( uv ), duv2 = dFdy( uv );
	vec3 N = normalize( cross( dp1, dp2 ) );
	if ( N.z < 0.0 ) N = -N;	// vers la camera
	vec3 dp2perp = cross( dp2, N ), dp1perp = cross( N, dp1 );
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt( max( dot( T, T ), dot( B, B ) ) + 1e-12 );
	T *= invmax; B *= invmax;
	vec2 g;
	if ( kind > 1.5 ) {	// eau, lave : des vagues
		vec3 w = ( invView * vec4( vp, 1.0 ) ).xyz;
		float sp = ( kind > 2.5 ) ? 0.35 : 1.0;
		float f = ( kind > 2.5 ) ? 0.012 : 0.03;
		float a = ( kind > 2.5 ) ? 0.2 : 0.16;
		g.x = ( sin( w.x * f + time * 1.1 * sp ) * 0.5 + sin( ( w.x + w.y ) * f * 1.7 - time * 1.9 * sp ) * 0.3 + sin( ( w.x * 0.7 - w.y * 1.9 ) * f * 3.1 + time * 2.7 * sp ) * 0.2 ) * a;
		g.y = ( sin( w.y * f * 1.3 - time * 1.4 * sp ) * 0.5 + sin( ( w.x - w.y ) * f * 2.1 + time * 1.6 * sp ) * 0.3 + sin( ( w.y * 0.6 + w.x * 2.3 ) * f * 2.9 - time * 2.3 * sp ) * 0.2 ) * a;
	} else {			// le relief de la texture
		float l = lum( uv - vec2( texel.x, 0.0 ) ), r = lum( uv + vec2( texel.x, 0.0 ) );
		float d = lum( uv - vec2( 0.0, texel.y ) ), u = lum( uv + vec2( 0.0, texel.y ) );
		g = vec2( r - l, u - d ) * strength * 2.5;
	}
	vec3 n = normalize( N - T * g.x - B * g.y );
	outMat = vec4( n * 0.5 + 0.5, ( kind + 1.0 + clamp( shine, 0.0, 0.9 ) ) / 5.0 );
}
)GLSL";

// Apres le transparent : le verre recoit un reflet trace, l'eau sa refraction
// et son reflet (fresnel), la lave sa lueur. La reflexion reprend l'image
// complete (avec le transparent), la refraction l'image d'avant (l'opaque
// derriere l'eau).
static const char *rtLateFragment = R"GLSL(#version 430 compatibility
uniform sampler2D mat;
uniform sampler2D matDepth;
uniform sampler2D sceneDepth;
uniform sampler2D scene;		// l'opaque, d'avant le transparent
uniform sampler2D current;		// l'image complete
uniform vec2 projAB;
uniform vec2 projCD;
uniform vec2 projEF;
uniform vec2 texel;
uniform mat4 invView;
uniform mat4 viewMat;
uniform float time;
uniform float reflStrength;
uniform int dynNodes;
layout(std430, binding = 0) readonly buffer B0 { vec4 wn[]; };
layout(std430, binding = 1) readonly buffer B1 { vec4 wt[]; };
layout(std430, binding = 2) readonly buffer B2 { vec4 dn[]; };
layout(std430, binding = 3) readonly buffer B3 { vec4 dt[]; };
in vec2 uv;
out vec4 outCol;
vec3 viewPosAt( vec2 t, float d ) {
	vec3 n = vec3( t * 2.0 - 1.0, d * 2.0 - 1.0 );
	float w = n.z / projEF.y + projEF.x / projEF.y;
	vec3 p = vec3( ( n.x + projCD.x ) / projAB.x, ( n.y + projCD.y ) / projAB.y, -1.0 );
	return p / w;
}
%s
vec3 reflectAt( vec3 p, vec3 rd, out float ok ) {
	ok = 0.0;
	float t = trace( p + rd * 0.5, rd, 8192.0, false, 8192.0 );
	if ( t >= 8192.0 ) return vec3( 0.0 );
	vec4 hv = viewMat * vec4( p + rd * t, 1.0 );
	if ( hv.z >= -1.0 ) return vec3( 0.0 );
	vec2 st = vec2( hv.x / -hv.z * projAB.x - projCD.x, hv.y / -hv.z * projAB.y - projCD.y ) * 0.5 + 0.5;
	if ( st.x <= 0.0 || st.x >= 1.0 || st.y <= 0.0 || st.y >= 1.0 ) return vec3( 0.0 );
	float sz = viewPosAt( st, texture( sceneDepth, st ).r ).z;
	if ( abs( sz - hv.z ) > 16.0 + 0.03 * -hv.z ) return vec3( 0.0 );
	ok = 1.0;
	return texture( current, st ).rgb;
}
void main() {
	vec4 m = texture( mat, uv );
	float kind = floor( m.a * 5.0 + 0.001 ) - 1.0;
	float md = texture( matDepth, uv ).r;
	float d = texture( sceneDepth, uv ).r;
	if ( kind < 0.5 || md >= 1.0 || md > d + 0.00005 ) discard;	// rien, ou cache par l'opaque
	vec3 cur = texture( current, uv ).rgb;
	vec3 pv = viewPosAt( uv, md );
	vec3 nv = normalize( m.xyz * 2.0 - 1.0 );
	vec3 p = ( invView * vec4( pv, 1.0 ) ).xyz;
	vec3 n = normalize( mat3( invView ) * nv );
	vec3 camPos = invView[3].xyz;
	vec3 vdir = normalize( p - camPos );
	float cosv = max( dot( -vdir, n ), 0.0 );
	float ok;
	if ( kind < 1.5 ) {			// verre : un reflet en plus
		vec3 refl = reflectAt( p, reflect( vdir, n ), ok );
		float fres = 0.15 + 0.85 * pow( 1.0 - cosv, 3.0 );
		outCol = vec4( cur + refl * ( ok * fres * 0.9 * reflStrength ), 1.0 );
	} else if ( kind < 2.5 ) {	// eau : refraction de ce qui est dessous, reflet dessus
		vec2 off = nv.xy * 0.035;
		vec3 under = texture( scene, clamp( uv + off, vec2( 0.002 ), vec2( 0.998 ) ) ).rgb;
		vec3 base = mix( cur, under * vec3( 0.75, 0.9, 1.0 ), 0.55 );
		vec3 refl = reflectAt( p, reflect( vdir, n ), ok );
		float fres = 0.08 + 0.92 * pow( 1.0 - cosv, 4.0 );
		if ( ok < 0.5 ) refl = mix( base, vec3( 0.45, 0.55, 0.65 ), 0.5 );
		float sun = pow( max( dot( reflect( vdir, n ), normalize( vec3( 0.45, 0.3, 0.9 ) ) ), 0.0 ), 160.0 );
		outCol = vec4( mix( base, refl, clamp( fres * reflStrength, 0.0, 0.85 ) ) + vec3( 0.35 ) * sun, 1.0 );
	} else {					// lave : ca brille, ca pulse
		float pulse = 0.85 + 0.25 * sin( time * 1.7 + p.x * 0.01 + p.y * 0.013 ) + 0.1 * nv.x;
		vec3 glow = vec3( 1.0, 0.45, 0.12 ) * ( 0.35 * pulse );
		outCol = vec4( cur * ( 1.1 + 0.3 * pulse ) + glow, 1.0 );
	}
}
)GLSL";

// ---------------------------------------------------------------- mise en place
static qboolean RT_Init( void )
{
	const char	*missing = NULL;

	if ( rtReady )
	{
		return qtrue;
	}
	if ( rtFailed )
	{
		return qfalse;
	}
	if ( !strstr( glConfig.extensions_string, "GL_ARB_shader_storage_buffer_object" ) )
	{
		ri.Printf( PRINT_ALL, "...ray tracing : la carte graphique ne fournit pas les tampons SSBO (GL 4.3), voie classique\n" );
		rtFailed = qtrue;
		return qfalse;
	}
#define GRAB( var, name ) \
	var = (decltype( var ))ri.GL_GetProcAddress( name ); \
	if ( !var ) { missing = name; }
	GRAB( p_glGenBuffers, "glGenBuffers" );
	GRAB( p_glDeleteBuffers, "glDeleteBuffers" );
	GRAB( p_glBindBuffer, "glBindBuffer" );
	GRAB( p_glBufferData, "glBufferData" );
	GRAB( p_glBindBufferBase, "glBindBufferBase" );
	GRAB( p_glDrawBuffers, "glDrawBuffers" );
	GRAB( p_glUniform4fv, "glUniform4fv" );
	GRAB( p_glUniform3fv, "glUniform3fv" );
#undef GRAB
	if ( missing )
	{
		ri.Printf( PRINT_ALL, "...ray tracing : %s manque, voie classique\n", missing );
		rtFailed = qtrue;
		return qfalse;
	}
	R_ModernDrainErrors();
	rtProg = R_ModernCompile( rtVertex, rtFragment, "ray tracing" );
	if ( !rtProg )
	{
		rtFailed = qtrue;
		return qfalse;
	}
	rtMatProg = R_ModernCompile( rtMatVertex, rtMatFragment, "materiaux" );
	{	// la passe tardive partage le parcours de la BVH avec la passe principale
		const char	*t0 = strstr( rtFragment, "#define TRAVERSE" );
		const char	*t1 = strstr( rtFragment, "void main() {" );

		if ( t0 && t1 && t1 > t0 )
		{
			char	*trav = (char *)malloc( t1 - t0 + 1 );
			char	*src = (char *)malloc( strlen( rtLateFragment ) + ( t1 - t0 ) + 16 );

			memcpy( trav, t0, t1 - t0 );
			trav[t1 - t0] = 0;
			Com_sprintf( src, strlen( rtLateFragment ) + ( t1 - t0 ) + 16, rtLateFragment, trav );
			rtLateProg = R_ModernCompile( rtVertex, src, "verre, eau, lave" );
			free( src );
			free( trav );
		}
	}
	if ( !rtMatProg || !rtLateProg )
	{
		rtFailed = qtrue;
		return qfalse;
	}
	p_glGenBuffers( 1, &rtWorldNodes );
	p_glGenBuffers( 1, &rtWorldTris );
	p_glGenBuffers( 1, &rtDynNodes );
	p_glGenBuffers( 1, &rtDynTris );
	RT_Upload( rtDynNodes, NULL, 0, GL_STREAM_DRAW );
	RT_Upload( rtDynTris, NULL, 0, GL_STREAM_DRAW );
	if ( !R_ModernStep( "tampons du ray tracing" ) )
	{
		rtFailed = qtrue;
		return qfalse;
	}
	rtReady = qtrue;
	ri.Printf( PRINT_ALL, "...ray tracing : pret\n" );
	return qtrue;
}

static GLuint RT_MakeTex( int w, int h, GLenum internal, GLenum format, GLenum type )
{
	GLuint	tex;

	qglGenTextures( 1, &tex );
	qglBindTexture( GL_TEXTURE_2D, tex );
	qglTexImage2D( GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	return tex;
}

static void RT_FreeTargets( void )
{
	if ( rtFbo )		{ p_glDeleteFramebuffers( 1, &rtFbo ); rtFbo = 0; }
	if ( rtMaskFbo )	{ p_glDeleteFramebuffers( 1, &rtMaskFbo ); rtMaskFbo = 0; }
	if ( rtLightTex )	{ qglDeleteTextures( 1, &rtLightTex ); rtLightTex = 0; }
	if ( rtAoTex )		{ qglDeleteTextures( 1, &rtAoTex ); rtAoTex = 0; }
	if ( rtReflTex )	{ qglDeleteTextures( 1, &rtReflTex ); rtReflTex = 0; }
	if ( rtMaskTex )	{ qglDeleteTextures( 1, &rtMaskTex ); rtMaskTex = 0; }
	if ( rtMaskDepth )	{ qglDeleteTextures( 1, &rtMaskDepth ); rtMaskDepth = 0; }
	if ( rtLateTex )	{ qglDeleteTextures( 1, &rtLateTex ); rtLateTex = 0; }
	rtWidth = rtHeight = 0;
	rtAoTexBound = 0;
}

static int		rtPassW, rtPassH;		// la resolution de la passe (r_modernRTScale)

static qboolean RT_Targets( int w, int h, GLuint aoTex )
{
	float	scale = r_modernRTScale->value;

	if ( scale < 0.25f ) scale = 0.25f;
	if ( scale > 1.0f ) scale = 1.0f;
	const int	pw = (int)( w * scale + 0.5f ), ph = (int)( h * scale + 0.5f );

	if ( w == rtWidth && h == rtHeight && pw == rtPassW && ph == rtPassH && rtFbo )
	{
		return qtrue;
	}
	RT_FreeTargets();
	R_ModernDrainErrors();
	rtPassW = pw;
	rtPassH = ph;
	rtAoTex = RT_MakeTex( pw, ph, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE );
	rtLightTex = RT_MakeTex( pw, ph, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE );
	rtReflTex = RT_MakeTex( pw, ph, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE );
	// lues a l'ecran en pleine resolution : en lineaire, sinon les marches
	{
		const GLuint smooth[3] = { rtAoTex, rtLightTex, rtReflTex };

		for ( int i = 0; i < 3; i++ )
		{
			qglBindTexture( GL_TEXTURE_2D, smooth[i] );
			qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		}
		qglBindTexture( GL_TEXTURE_2D, 0 );
	}
	rtMaskTex = RT_MakeTex( w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE );
	rtMaskDepth = RT_MakeTex( w, h, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT );
	rtLateTex = RT_MakeTex( w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE );

	p_glGenFramebuffers( 1, &rtFbo );
	p_glBindFramebuffer( GL_FRAMEBUFFER, rtFbo );
	p_glFramebufferTexture2D( GL_FRAMEBUFFER, RT_COLOR_ATTACHMENT( 0 ), GL_TEXTURE_2D, rtAoTex, 0 );
	p_glFramebufferTexture2D( GL_FRAMEBUFFER, RT_COLOR_ATTACHMENT( 1 ), GL_TEXTURE_2D, rtLightTex, 0 );
	p_glFramebufferTexture2D( GL_FRAMEBUFFER, RT_COLOR_ATTACHMENT( 2 ), GL_TEXTURE_2D, rtReflTex, 0 );
	{
		const GLenum bufs[3] = { RT_COLOR_ATTACHMENT( 0 ), RT_COLOR_ATTACHMENT( 1 ), RT_COLOR_ATTACHMENT( 2 ) };
		p_glDrawBuffers( 3, bufs );
	}
	const qboolean ok1 = (qboolean)( p_glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE );

	p_glGenFramebuffers( 1, &rtMaskFbo );
	p_glBindFramebuffer( GL_FRAMEBUFFER, rtMaskFbo );
	p_glFramebufferTexture2D( GL_FRAMEBUFFER, RT_COLOR_ATTACHMENT( 0 ), GL_TEXTURE_2D, rtMaskTex, 0 );
	p_glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, rtMaskDepth, 0 );
	const qboolean ok2 = (qboolean)( p_glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE );
	p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	if ( !ok1 || !ok2 || !R_ModernStep( "cibles du ray tracing" ) )
	{
		ri.Printf( PRINT_ALL, "...ray tracing : cibles incompletes, voie classique\n" );
		RT_FreeTargets();
		rtFailed = qtrue;
		return qfalse;
	}
	rtWidth = w;
	rtHeight = h;
	rtAoTexBound = aoTex;
	return qtrue;
}

static qboolean RT_ShaderShines( const shader_t *sh );

static void RT_EnsureWorld( void )
{
	if ( rtWorldBuilt == tr.world && rtWorldSurfaces == tr.world->numsurfaces && !Q_stricmp( rtWorldName, tr.world->name ) )
	{
		return;
	}
	std::vector<rtTri>	tris;
	const int			t0 = ri.Milliseconds();

	RT_GatherWorld( tris );
	RT_Build( tris, rtNodesTmp, rtTrisTmp );
	{	// les nappes de lave : regroupees par cases de 384 unites, elles
		// eclaireront alentour comme des lumieres tracees
		const bmodel_t	*world = &tr.world->bmodels[0];
		struct cell { int cx, cy, cz; vec3_t sum; float area; int n; };
		std::vector<cell> cells;

		for ( int i = 0; i < world->numSurfaces; i++ )
		{
			const msurface_t *surf = &world->firstSurface[i];

			if ( !surf->shader || !( surf->shader->contentFlags & CONTENTS_LAVA ) || !surf->data || *surf->data != SF_FACE )
			{
				continue;
			}
			const srfSurfaceFace_t *face = (const srfSurfaceFace_t *)surf->data;
			vec3_t c = { 0, 0, 0 };

			for ( int k = 0; k < face->numPoints; k++ )
			{
				VectorAdd( c, face->points[k], c );
			}
			VectorScale( c, 1.0f / face->numPoints, c );
			const int cx = (int)floorf( c[0] / 384.0f ), cy = (int)floorf( c[1] / 384.0f ), cz = (int)floorf( c[2] / 384.0f );
			size_t j;

			for ( j = 0; j < cells.size(); j++ )
			{
				if ( cells[j].cx == cx && cells[j].cy == cy && cells[j].cz == cz ) break;
			}
			if ( j == cells.size() )
			{
				cell nc = { cx, cy, cz, { 0, 0, 0 }, 0.0f, 0 };
				cells.push_back( nc );
			}
			VectorAdd( cells[j].sum, c, cells[j].sum );
			cells[j].n++;
		}
		rtLavaCount = 0;
		for ( size_t j = 0; j < cells.size() && rtLavaCount < RT_MAX_LAVA; j++ )
		{
			VectorScale( cells[j].sum, 1.0f / cells[j].n, rtLava[rtLavaCount] );
			rtLava[rtLavaCount][2] += 24.0f;		// un peu au-dessus de la nappe
			rtLavaSize[rtLavaCount] = (float)cells[j].n;
			rtLavaCount++;
		}
	}
	RT_Upload( rtWorldNodes, rtNodesTmp.data(), rtNodesTmp.size() * sizeof( rtNode ), GL_STATIC_DRAW );
	RT_Upload( rtWorldTris, rtTrisTmp.data(), rtTrisTmp.size() * sizeof( float ), GL_STATIC_DRAW );
	rtWorldNodeCount = (int)rtNodesTmp.size();
	rtWorldTriCount = (int)tris.size();
	rtWorldBuilt = tr.world;
	rtWorldSurfaces = tr.world->numsurfaces;
	Q_strncpyz( rtWorldName, tr.world->name, sizeof( rtWorldName ) );
	ri.Printf( PRINT_ALL, "...ray tracing : decor de %s, %i triangles, %i noeuds, %i ms\n",
		tr.world->name, rtWorldTriCount, rtWorldNodeCount, ri.Milliseconds() - t0 );
	{	// les surfaces brillantes : ou les reflets peuvent se voir
		const bmodel_t	*world = &tr.world->bmodels[0];
		int				shiny = 0, said = 0;
		vec3_t			where = { 0, 0, 0 };

		for ( int i = 0; i < world->numSurfaces; i++ )
		{
			const msurface_t *surf = &world->firstSurface[i];

			if ( surf->shader && RT_ShaderShines( surf->shader ) )
			{
				shiny++;
				if ( said < 6 )
				{
					said++;
					if ( surf->data && *surf->data == SF_FACE )
					{
						VectorCopy( ( (const srfSurfaceFace_t *)surf->data )->points[0], where );
					}
					ri.Printf( PRINT_ALL, "   brillante : %s vers %.0f %.0f %.0f\n", surf->shader->name, where[0], where[1], where[2] );
				}
			}
		}
		int water = 0, glass = 0, lava = 0;
		vec3_t wpos = { 0, 0, 0 }, wmin = { 1e30f, 1e30f, 1e30f }, wmax = { -1e30f, -1e30f, -1e30f };

		for ( int i = 0; i < world->numSurfaces; i++ )
		{
			const msurface_t *surf = &world->firstSurface[i];
			const int k = surf->shader ? RT_ShaderKind( surf->shader ) : -1;

			if ( k == 2 )
			{
				water++;
				if ( surf->data && *surf->data == SF_FACE )
				{
					const srfSurfaceFace_t *f = (const srfSurfaceFace_t *)surf->data;
					for ( int v = 0; v < f->numPoints; v++ )
					{
						for ( int a = 0; a < 3; a++ )
						{
							if ( f->points[v][a] < wmin[a] ) wmin[a] = f->points[v][a];
							if ( f->points[v][a] > wmax[a] ) wmax[a] = f->points[v][a];
						}
					}
				}
			}
			else if ( k == 1 ) glass++;
			else if ( k == 3 ) lava++;
		}
		if ( water ) { VectorAdd( wmin, wmax, wpos ); VectorScale( wpos, 0.5f, wpos ); }
		ri.Printf( PRINT_ALL, "   %i surfaces brillantes, %i d'eau (boite %.0f %.0f %.0f a %.0f %.0f %.0f, centre %.0f %.0f %.0f), %i de verre, %i de lave (%i nappes) dans le decor\n",
			shiny, water, wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2], wpos[0], wpos[1], wpos[2], glass, lava, rtLavaCount );
	}
}

// ---------------------------------------------------------------- l'interface
qboolean R_ModernRTActive( void )
{
	return (qboolean)( r_modernRT && r_modernRT->integer && r_modern && r_modern->integer && !rtFailed );
}

qboolean R_ModernRTLightsActive( void )
{
	return (qboolean)( R_ModernRTActive() && rtReady && r_modernRTLights->integer );
}

GLuint R_ModernRTLightTex( void ) { return rtLightTex; }
GLuint R_ModernRTReflTex( void ) { return rtReflTex; }
GLuint R_ModernRTAoTex( void ) { return rtAoTex; }
GLuint R_ModernRTMatTex( void ) { return rtMaskTex; }
GLuint R_ModernRTMatDepth( void ) { return rtMaskDepth; }

void R_ModernRTShutdown( void )
{
	if ( rtProg )
	{
		p_glDeleteProgram( rtProg );
		rtProg = 0;
	}
	if ( rtMatProg )
	{
		p_glDeleteProgram( rtMatProg );
		rtMatProg = 0;
	}
	if ( rtLateProg )
	{
		p_glDeleteProgram( rtLateProg );
		rtLateProg = 0;
	}
	if ( rtReady )
	{
		RT_FreeTargets();
		GLuint bufs[4] = { rtWorldNodes, rtWorldTris, rtDynNodes, rtDynTris };
		p_glDeleteBuffers( 4, bufs );
		rtWorldNodes = rtWorldTris = rtDynNodes = rtDynTris = 0;
	}
	rtReady = qfalse;
	rtFailed = qfalse;
	rtWorldBuilt = NULL;
	rtViewActive = qfalse;
	rtDyn.clear();
}

// Debut de la vue principale : on repart de zero pour la capture, et le masque
// des surfaces brillantes est efface.
void R_ModernRTViewBegin( void )
{
	rtViewActive = qfalse;
	rtLateActive = qfalse;
	if ( !R_ModernRTActive() || !rtReady || !rtMaskFbo || g_bRenderGlowingObjects )
	{
		return;
	}
	if ( ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL ) ) || backEnd.viewParms.isPortal )
	{
		return;
	}
	rtViewActive = qtrue;
	rtDyn.clear();
	rtDynEnts.clear();
	rtDynLastEnt = NULL;
	p_glBindFramebuffer( GL_FRAMEBUFFER, rtMaskFbo );
	qglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

static qboolean RT_ShaderShines( const shader_t *sh )
{
	if ( !sh->stages )
	{
		return qfalse;
	}
	for ( int i = 0; i < sh->numUnfoggedPasses; i++ )
	{
		if ( sh->stages[i].bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED )
		{
			return qtrue;
		}
	}
	return qfalse;
}

// Une surface du decor dans le tampon de materiaux : normale de relief,
// genre, brillance, avec sa profondeur.
static void RT_DrawMaterial( const shader_t *sh, int kind, float shine )
{
	const image_t	*img = RT_ShaderDiffuse( sh );
	const uint32_t	bits = glState.glStateBits;

	if ( !img && kind == 0 )
	{
		return;
	}
	p_glBindFramebuffer( GL_FRAMEBUFFER, rtMaskFbo );
	GL_State( GLS_DEFAULT );
	GL_SelectTexture( 1 );
	qglDisable( GL_TEXTURE_2D );
	GL_SelectTexture( 0 );
	if ( img )
	{
		GL_Bind( (image_t *)img );
	}
	qglDisableClientState( GL_COLOR_ARRAY );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglTexCoordPointer( 2, GL_FLOAT, sizeof( vec2_t ) * NUM_TEX_COORDS, tess.texCoords[0][0] );
	qglVertexPointer( 3, GL_FLOAT, 16, tess.xyz );

	p_glUseProgram( rtMatProg );
	p_glUniform1i( p_glGetUniformLocation( rtMatProg, "diffuse" ), 0 );
	p_glUniform2f( p_glGetUniformLocation( rtMatProg, "texel" ), img ? 1.0f / img->width : 0.001f, img ? 1.0f / img->height : 0.001f );
	p_glUniform1f( p_glGetUniformLocation( rtMatProg, "strength" ), r_modernRTNormals->integer ? r_modernRTNormalStrength->value : 0.0f );
	p_glUniform1f( p_glGetUniformLocation( rtMatProg, "kind" ), (float)kind );
	p_glUniform1f( p_glGetUniformLocation( rtMatProg, "shine" ), shine );
	p_glUniform1f( p_glGetUniformLocation( rtMatProg, "time" ), backEnd.refdef.time * 0.001f );
	{
		float	invView[16];
		const float	*m = backEnd.viewParms.world.modelMatrix;

		for ( int r = 0; r < 3; r++ )
		{
			for ( int c = 0; c < 3; c++ ) invView[c * 4 + r] = m[r * 4 + c];
			invView[12 + r] = -( m[12] * m[r * 4 + 0] + m[13] * m[r * 4 + 1] + m[14] * m[r * 4 + 2] );
		}
		invView[3] = invView[7] = invView[11] = 0.0f;
		invView[15] = 1.0f;
		p_glUniformMatrix4fv( p_glGetUniformLocation( rtMatProg, "invView" ), 1, GL_FALSE, invView );
	}
	qglDrawElements( GL_TRIANGLES, tess.numIndexes, GL_UNSIGNED_INT, tess.indexes );
	p_glUseProgram( 0 );

	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	GL_State( bits );
	p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

// Apres chaque surface dessinee (RB_EndSurface) : les sous-modeles et les
// personnages deposent leurs triangles dans le monde ; le decor (monde et
// sous-modeles, pas les persos) se redessine dans le tampon de materiaux ;
// apres la passe, le verre, l'eau et la lave y entrent a leur tour.
void R_ModernRTAfterSurface( void )
{
	if ( g_bRenderGlowingObjects || tess.numIndexes < 3 || !rtReady )
	{
		return;
	}
	const shader_t *sh = tess.shader;

	if ( !sh || sh == tr.shadowShader || sh == tr.projectionShadowShader || ( sh->surfaceFlags & ( SURF_NODRAW | SURF_SKY ) ) )
	{
		return;
	}
	const refEntity_t	*e = &backEnd.currentEntity->e;
	const qboolean		isWorld = (qboolean)( backEnd.currentEntity == &tr.worldEntity );
	const qboolean		isBrush = (qboolean)( !isWorld && e->reType == RT_MODEL && e->hModel && R_GetModelByHandle( e->hModel )->type == MOD_BRUSH );
	const int			kind = RT_ShaderKind( sh );

	if ( rtLateActive && !rtViewActive )
	{	// apres la passe : seulement le verre, l'eau, la lave du decor
		if ( kind >= 1 && ( isWorld || isBrush ) && !( e->renderfx & RF_DEPTHHACK ) )
		{
			RT_DrawMaterial( sh, kind, 0.0f );
		}
		return;
	}
	if ( !rtViewActive || sh->sort > SS_OPAQUE )
	{
		return;
	}

	if ( r_modernRTDynamic->integer && !isWorld
		&& (int)rtDyn.size() + tess.numIndexes / 3 <= RT_MAX_DYN_TRIS )
	{
		if ( !( e->renderfx & ( RF_DEPTHHACK | RF_NOSHADOW ) ) )
		{
			vec3_t	w[3];

			if ( rtDynLastEnt != (const void *)backEnd.currentEntity || rtDynEnts.empty() )
			{	// une nouvelle entite : un nouveau groupe contigu
				rtRange r;

				r.first = (int)rtDyn.size();
				r.count = 0;
				rtDynEnts.push_back( r );
				rtDynLastEnt = backEnd.currentEntity;
			}
			for ( int i = 0; i + 2 < tess.numIndexes; i += 3 )
			{
				for ( int k = 0; k < 3; k++ )
				{
					const float *v = tess.xyz[tess.indexes[i + k]];

					VectorMA( e->origin, v[0], e->axis[0], w[k] );
					VectorMA( w[k], v[1], e->axis[1], w[k] );
					VectorMA( w[k], v[2], e->axis[2], w[k] );
				}
				RT_PushTri( rtDyn, w[0], w[1], w[2] );
				rtDynEnts.back().count++;
			}
		}
	}

	if ( kind >= 0 && ( isWorld || isBrush ) )
	{	// le decor, avec son relief et sa brillance (lave et eau opaques comprises)
		RT_DrawMaterial( sh, kind, ( kind == 0 && r_modernRTReflect->integer && RT_ShaderShines( sh ) ) ? 0.6f : 0.0f );
	}
}

// La couleur d'une lame, d'apres le shader de sa lueur (les couleurs du jeu)
// et sa teinte (la couleur exacte choisie dans le salon).
static void RT_SaberColor( const refEntity_t *e, vec3_t out )
{
	const shader_t	*sh = R_GetShaderByHandle( e->customShader );
	vec3_t			base = { 1.0f, 1.0f, 1.0f };

	if ( sh )
	{
		if ( strstr( sh->name, "red" ) )			VectorSet( base, 1.0f, 0.25f, 0.2f );
		else if ( strstr( sh->name, "orange" ) )	VectorSet( base, 1.0f, 0.55f, 0.1f );
		else if ( strstr( sh->name, "yellow" ) )	VectorSet( base, 1.0f, 0.95f, 0.25f );
		else if ( strstr( sh->name, "green" ) )		VectorSet( base, 0.25f, 1.0f, 0.3f );
		else if ( strstr( sh->name, "blue" ) )		VectorSet( base, 0.3f, 0.45f, 1.0f );
		else if ( strstr( sh->name, "purple" ) )	VectorSet( base, 0.8f, 0.3f, 1.0f );
	}
	out[0] = base[0] * e->shaderRGBA[0] / 255.0f;
	out[1] = base[1] * e->shaderRGBA[1] / 255.0f;
	out[2] = base[2] * e->shaderRGBA[2] / 255.0f;
}

// Apres le transparent (au premier 2D) : verre, eau, lave. L'image complete
// est capturee pour les reflets ; l'opaque d'avant (sceneTex) sert a la
// refraction de l'eau.
void R_ModernRTLatePass( const modernRTParams_t *p )
{
	if ( !rtLateActive || !rtReady || !rtLateProg )
	{
		rtLateActive = qfalse;
		return;
	}
	rtLateActive = qfalse;
	if ( !r_modernRTWater->integer && !r_modernRTGlass->integer && !r_modernRTLava->integer )
	{
		return;
	}
	const float	*P = p->proj;

	R_ModernDrainErrors();
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, rtLateTex );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, p->w, p->h );

	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();
	GL_State( GLS_DEPTHTEST_DISABLE );
	GL_Cull( CT_TWO_SIDED );
	qglViewport( 0, 0, p->w, p->h );
	qglScissor( 0, 0, p->w, p->h );

	GL_SelectTexture( 3 );
	qglBindTexture( GL_TEXTURE_2D, p->sceneTex );
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, p->depthTex );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, rtMaskDepth );
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, rtMaskTex );
	qglActiveTextureARB( 0x84C4 /* GL_TEXTURE4_ARB */ );
	qglBindTexture( GL_TEXTURE_2D, rtLateTex );
	qglActiveTextureARB( GL_TEXTURE0_ARB );

	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 0, rtWorldNodes );
	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 1, rtWorldTris );
	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 2, rtDynNodes );
	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 3, rtDynTris );

	p_glUseProgram( rtLateProg );
#define U( name )	p_glGetUniformLocation( rtLateProg, name )
	p_glUniform1i( U( "mat" ), 0 );
	p_glUniform1i( U( "matDepth" ), 1 );
	p_glUniform1i( U( "sceneDepth" ), 2 );
	p_glUniform1i( U( "scene" ), 3 );
	p_glUniform1i( U( "current" ), 4 );
	p_glUniform2f( U( "projAB" ), P[0], P[5] );
	p_glUniform2f( U( "projCD" ), P[8], P[9] );
	p_glUniform2f( U( "projEF" ), P[10], P[14] );
	p_glUniform2f( U( "texel" ), 1.0f / (float)p->w, 1.0f / (float)p->h );
	p_glUniformMatrix4fv( U( "invView" ), 1, GL_FALSE, p->invView );
	p_glUniformMatrix4fv( U( "viewMat" ), 1, GL_FALSE, p->view );
	p_glUniform1f( U( "time" ), backEnd.refdef.time * 0.001f );
	p_glUniform1f( U( "reflStrength" ), r_modernRTReflectStrength->value );
	p_glUniform1i( U( "dynNodes" ), rtDynNodeCount );
#undef U
	R_ModernFullscreenQuad();
	p_glUseProgram( 0 );

	qglActiveTextureARB( 0x84C4 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	qglActiveTextureARB( GL_TEXTURE0_ARB );
	GL_SelectTexture( 3 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	glState.currenttextures[0] = glState.currenttextures[1] = -1;

	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();
	R_ModernStep( "verre, eau, lave" );
}

qboolean R_ModernRTPass( const modernRTParams_t *p )
{
	if ( !R_ModernRTActive() || !tr.world )
	{
		return qfalse;
	}
	if ( !RT_Init() || !RT_Targets( p->w, p->h, p->aoTex ) )
	{
		return qfalse;
	}
	// A l'allumage : l'ombre volumetrique du jeu n'a plus de sens, le trace
	// fait l'ombre des personnages (JD). On la coupe une fois ; le joueur peut
	// la remettre, elle restera alors ignoree tant que le trace est actif.
	if ( r_modernRT->modificationCount != rtLastMod )
	{
		rtLastMod = r_modernRT->modificationCount;
		if ( ri.Cvar_VariableIntegerValue( "cg_shadows" ) != 0 )
		{
			ri.Cvar_Set( "cg_shadows", "0" );
			ri.Printf( PRINT_ALL, "...ray tracing : ombre volumetrique du jeu coupee (cg_shadows 0), le trace s'en charge\n" );
		}
	}
	RT_EnsureWorld();

	// --- les sous-modeles et personnages de cette image
	const int	tCpu0 = ri.Milliseconds();

	rtDynNodeCount = 0;
	if ( r_modernRTDynamic->integer && !rtDyn.empty() )
	{
		RT_Build( rtDyn, rtNodesTmp, rtTrisTmp, &rtDynEnts );
		RT_Upload( rtDynNodes, rtNodesTmp.data(), rtNodesTmp.size() * sizeof( rtNode ), GL_STREAM_DRAW );
		RT_Upload( rtDynTris, rtTrisTmp.data(), rtTrisTmp.size() * sizeof( float ), GL_STREAM_DRAW );
		rtDynNodeCount = (int)rtNodesTmp.size();
	}
	rtViewActive = qfalse;		// la capture de cette vue est close
	rtLateActive = qtrue;		// ... et celle du verre, de l'eau, de la lave s'ouvre
	rtLateFrame = backEnd.refdef.time;
	const int	tCpu1 = ri.Milliseconds();

	// --- les lumieres : celles de la scene, et les lames de sabre
	float	lPos[RT_MAX_LIGHTS][4], lCol[RT_MAX_LIGHTS][4], lEnd[RT_MAX_LIGHTS][4];
	int		numLights = 0;

	if ( r_modernRTLights->integer )
	{
		for ( int i = 0; i < backEnd.refdef.num_dlights && numLights < RT_MAX_LIGHTS; i++ )
		{
			const dlight_t *dl = &backEnd.refdef.dlights[i];

			VectorCopy( dl->origin, lPos[numLights] );
			lPos[numLights][3] = dl->radius;
			VectorCopy( dl->color, lCol[numLights] );
			lCol[numLights][3] = 0.0f;
			VectorCopy( dl->origin, lEnd[numLights] );
			lEnd[numLights][3] = 0.0f;
			numLights++;
		}
		if ( r_modernRTLava->integer )
		{	// les nappes de lave les plus proches de la camera
			const float	*cam = backEnd.viewParms.ori.origin;

			for ( int pass = 0; pass < 12 && numLights < RT_MAX_LIGHTS; pass++ )
			{
				int		best = -1;
				float	bestD = 1e30f;

				for ( int i = 0; i < rtLavaCount; i++ )
				{
					vec3_t	dv;
					qboolean used = qfalse;

					for ( int k = 0; k < numLights; k++ )
					{
						if ( lCol[k][3] > 1.5f && lPos[k][0] == rtLava[i][0] && lPos[k][1] == rtLava[i][1] && lPos[k][2] == rtLava[i][2] ) { used = qtrue; break; }
					}
					if ( used ) continue;
					VectorSubtract( rtLava[i], cam, dv );
					const float dist = VectorLength( dv );

					if ( dist < bestD ) { bestD = dist; best = i; }
				}
				if ( best < 0 || bestD > 1600.0f )
				{
					break;
				}
				VectorCopy( rtLava[best], lPos[numLights] );
				lPos[numLights][3] = 320.0f + 40.0f * ( rtLavaSize[best] > 6.0f ? 6.0f : rtLavaSize[best] );
				VectorSet( lCol[numLights], 1.0f, 0.42f, 0.1f );
				lCol[numLights][3] = 2.0f;		// > 1.5 : une nappe de lave (point)
				VectorCopy( rtLava[best], lEnd[numLights] );
				lEnd[numLights][3] = 0.0f;
				numLights++;
			}
		}
		for ( int i = 0; i < backEnd.refdef.num_entities && numLights < RT_MAX_LIGHTS; i++ )
		{
			const refEntity_t *e = &backEnd.refdef.entities[i].e;

			if ( e->reType != RT_SABER_GLOW || e->saberLength < 1.0f )
			{
				continue;
			}
			VectorCopy( e->origin, lPos[numLights] );
			lPos[numLights][3] = 160.0f + e->saberLength * 2.0f;		// la diffusion de la lame
			RT_SaberColor( e, lCol[numLights] );
			lCol[numLights][3] = 1.0f;									// un segment
			VectorMA( e->origin, e->saberLength, e->axis[0], lEnd[numLights] );
			lEnd[numLights][3] = 0.0f;
			numLights++;
		}
	}

	// --- la passe
	const float	*P = p->proj;
	const int	sunRays = r_modernRTSunRays->integer < 1 ? 1 : ( r_modernRTSunRays->integer > 16 ? 16 : r_modernRTSunRays->integer );
	const int	aoRays = r_modernRTAORays->integer < 1 ? 1 : ( r_modernRTAORays->integer > 32 ? 32 : r_modernRTAORays->integer );

	R_ModernDrainErrors();
	if ( r_modernDebug->integer == 6 )
	{
		qglFinish();
	}
	const int	tGpu0 = ri.Milliseconds();

	p_glBindFramebuffer( GL_FRAMEBUFFER, rtFbo );
	qglViewport( 0, 0, rtPassW, rtPassH );
	qglScissor( 0, 0, rtPassW, rtPassH );

	GL_SelectTexture( 3 );
	qglBindTexture( GL_TEXTURE_2D, rtMaskDepth );
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, rtMaskTex );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, p->sceneTex );
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, p->depthTex );

	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 0, rtWorldNodes );
	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 1, rtWorldTris );
	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 2, rtDynNodes );
	p_glBindBufferBase( GL_SHADER_STORAGE_BUFFER, 3, rtDynTris );

	p_glUseProgram( rtProg );
#define U( name )	p_glGetUniformLocation( rtProg, name )
	p_glUniform1i( U( "sceneDepth" ), 0 );
	p_glUniform1i( U( "scene" ), 1 );
	p_glUniform1i( U( "shineMask" ), 2 );
	p_glUniform1i( U( "shineDepth" ), 3 );
	p_glUniform2f( U( "projAB" ), P[0], P[5] );
	p_glUniform2f( U( "projCD" ), P[8], P[9] );
	p_glUniform2f( U( "projEF" ), P[10], P[14] );
	p_glUniform2f( U( "texel" ), 1.0f / (float)p->w, 1.0f / (float)p->h );
	p_glUniformMatrix4fv( U( "invView" ), 1, GL_FALSE, p->invView );
	p_glUniformMatrix4fv( U( "viewMat" ), 1, GL_FALSE, p->view );
	p_glUniform3f( U( "sunDir" ), p->sunDir[0], p->sunDir[1], p->sunDir[2] );
	p_glUniform1f( U( "sunOn" ), p->wantSun ? 1.0f : 0.0f );
	p_glUniform1f( U( "sunSoft" ), 0.012f * ( r_modernRTSoft->value < 0.0f ? 0.0f : r_modernRTSoft->value ) );
	p_glUniform1i( U( "sunRays" ), sunRays );
	p_glUniform1f( U( "aoOn" ), p->wantAo ? 1.0f : 0.0f );
	p_glUniform1f( U( "aoRange" ), r_modernRTAORange->value > 8.0f ? r_modernRTAORange->value : 8.0f );
	p_glUniform1i( U( "aoRays" ), aoRays );
	p_glUniform1i( U( "numLights" ), numLights );
	if ( numLights )
	{
		p_glUniform4fv( U( "lPos" ), numLights, &lPos[0][0] );
		p_glUniform4fv( U( "lCol" ), numLights, &lCol[0][0] );
		p_glUniform4fv( U( "lEnd" ), numLights, &lEnd[0][0] );
	}
	p_glUniform1f( U( "lightScale" ), r_modernRTLightScale->value );
	p_glUniform1f( U( "reflOn" ), r_modernRTReflect->integer ? 1.0f : 0.0f );
	p_glUniform1f( U( "reflStrength" ), r_modernRTReflectStrength->value );
	p_glUniform1i( U( "dynNodes" ), rtDynNodeCount );
#undef U
	R_ModernFullscreenQuad();
	p_glUseProgram( 0 );

	GL_SelectTexture( 3 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 0 );
	p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	if ( r_modernDebug->integer == 6 )
	{
		static int lastSay;

		qglFinish();
		const int	tGpu1 = ri.Milliseconds();

		if ( abs( (int)( backEnd.refdef.time - lastSay ) ) > 1000 )
		{
			lastSay = backEnd.refdef.time;
			ri.Printf( PRINT_ALL, "ray tracing: %i triangles dynamiques (%i noeuds), %i lumieres, soleil %i rayons, occlusion %i rayons | CPU bvh %i ms, GPU passe %i ms, %ix%i\n",
				(int)rtDyn.size(), rtDynNodeCount, numLights, p->wantSun ? sunRays : 0, p->wantAo ? aoRays : 0,
				tCpu1 - tCpu0, tGpu1 - tGpu0, rtPassW, rtPassH );
			{	// ce que la scene contient
				int hist[16] = { 0 };
				for ( int i = 0; i < backEnd.refdef.num_entities; i++ )
				{
					const int rt = backEnd.refdef.entities[i].e.reType;
					if ( rt >= 0 && rt < 16 ) hist[rt]++;
					if ( rt == RT_SABER_GLOW ) ri.Printf( PRINT_ALL, "  lueur de sabre : longueur %.1f shader %i\n", backEnd.refdef.entities[i].e.saberLength, backEnd.refdef.entities[i].e.customShader );
				}
				ri.Printf( PRINT_ALL, "  entites %i : types", backEnd.refdef.num_entities );
				for ( int k = 0; k < 16; k++ ) if ( hist[k] ) ri.Printf( PRINT_ALL, " %i:%i", k, hist[k] );
				ri.Printf( PRINT_ALL, "  dlights %i\n", backEnd.refdef.num_dlights );
			}
			for ( int i = 0; i < numLights; i++ )
			{
				ri.Printf( PRINT_ALL, "  lumiere %i : %.0f %.0f %.0f rayon %.0f couleur %.2f %.2f %.2f %s\n", i,
					lPos[i][0], lPos[i][1], lPos[i][2], lPos[i][3], lCol[i][0], lCol[i][1], lCol[i][2], lCol[i][3] > 0.5f ? "(lame)" : "" );
			}
			{	// les trois cibles, relues
				const GLuint	texs[3] = { rtAoTex, rtLightTex, rtReflTex };
				const char		*names[3] = { "occlusion/ombre", "lumiere", "reflet" };
				byte			*buf = (byte *)malloc( (size_t)rtPassW * rtPassH * 4 );

				for ( int t = 0; t < 3 && buf; t++ )
				{
					int	mn[4] = { 255, 255, 255, 255 }, mx[4] = { 0, 0, 0, 0 };
					double	avg[4] = { 0, 0, 0, 0 };
					const int	n = rtPassW * rtPassH;

					qglBindTexture( GL_TEXTURE_2D, texs[t] );
					qglGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf );
					for ( int k = 0; k < n; k++ )
					{
						for ( int c = 0; c < 4; c++ )
						{
							const int v = buf[k * 4 + c];
							if ( v < mn[c] ) mn[c] = v;
							if ( v > mx[c] ) mx[c] = v;
							avg[c] += v;
						}
					}
					ri.Printf( PRINT_ALL, "  cible %-16s r %3i..%3i (%.0f)  g %3i..%3i (%.0f)  b %3i..%3i (%.0f)  a %3i..%3i (%.0f)\n", names[t],
						mn[0], mx[0], avg[0] / n, mn[1], mx[1], avg[1] / n, mn[2], mx[2], avg[2] / n, mn[3], mx[3], avg[3] / n );
				}
				qglBindTexture( GL_TEXTURE_2D, 0 );
				free( buf );
			}
		}
	}
	if ( !R_ModernStep( "passe de ray tracing" ) )
	{
		rtFailed = qtrue;
		return qfalse;
	}
	return qtrue;
}
