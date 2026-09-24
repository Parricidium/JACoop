/*
===========================================================================
JACoop - rendu moderne (couche additive)

Le moteur solo de Jedi Academy dessine en OpenGL a pipeline fixe : pas un seul
shader, a part deux vieux programmes ARB pour le halo. Impossible d'y poser des
ombres, de la lumiere volumetrique ou de l'occlusion ambiante sans une couche
capable de faire tourner du GLSL.

Le parti pris est celui d'ArxModern : on ne REMPLACE rien. La scene continue
d'etre dessinee exactement comme avant par le pipeline d'origine ; cette couche
recupere l'image finie et sa profondeur, puis applique ses passes par-dessus.

	r_modern 0   ->  aucun tampon cree, aucun shader compile, aucune passe :
	                 le chemin de code est celui d'avant, a l'octet pres.
	r_modern 1   ->  la couche s'allume, chaque effet garde son propre bouton.

Si la carte ou le pilote ne suit pas, on retombe tout seul sur le rendu
d'origine plutot que de refuser de demarrer (le vieux portable d'un ami, une
machine virtuelle).
===========================================================================
*/

#include "tr_local.h"
#include "tr_modern.h"

cvar_t	*r_modern;
cvar_t	*r_modernDebug;
cvar_t	*r_modernAO;
cvar_t	*r_modernAOIntensity;
cvar_t	*r_modernAORadius;
cvar_t	*r_modernSun;
cvar_t	*r_modernSunStrength;
cvar_t	*r_modernSunLength;
cvar_t	*r_modernRays;
cvar_t	*r_modernRaysStrength;
cvar_t	*r_modernSunMap;
cvar_t	*r_modernSunMapRange;

// ---------------------------------------------------------------- entrees GL
// Le moteur ne connait que le GL 1.x : on va chercher nous-memes ce qu'il faut.
// Les pilotes de bureau donnent deja un contexte de compatibilite recent, donc
// il n'y a en general rien a demander a SDL - on verifie, c'est tout.


PFN_glGenFramebuffers		p_glGenFramebuffers;
PFN_glDeleteFramebuffers		p_glDeleteFramebuffers;
PFN_glBindFramebuffer		p_glBindFramebuffer;
PFN_glFramebufferTexture2D	p_glFramebufferTexture2D;
PFN_glCheckFramebufferStatus	p_glCheckFramebufferStatus;
PFN_glCreateShader			p_glCreateShader;
PFN_glShaderSource			p_glShaderSource;
PFN_glCompileShader			p_glCompileShader;
PFN_glGetShaderiv			p_glGetShaderiv;
PFN_glGetShaderInfoLog		p_glGetShaderInfoLog;
PFN_glDeleteShader			p_glDeleteShader;
PFN_glCreateProgram			p_glCreateProgram;
PFN_glAttachShader			p_glAttachShader;
PFN_glLinkProgram			p_glLinkProgram;
PFN_glGetProgramiv			p_glGetProgramiv;
PFN_glGetProgramInfoLog		p_glGetProgramInfoLog;
PFN_glDeleteProgram			p_glDeleteProgram;
PFN_glUseProgram				p_glUseProgram;
PFN_glGetUniformLocation		p_glGetUniformLocation;
PFN_glUniform1i				p_glUniform1i;
PFN_glUniform1f				p_glUniform1f;
PFN_glUniform2f				p_glUniform2f;
PFN_glUniform3f				p_glUniform3f;
PFN_glUniform4f				p_glUniform4f;
PFN_glUniformMatrix4fv		p_glUniformMatrix4fv;

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER			0x8D40
#define GL_COLOR_ATTACHMENT0	0x8CE0
#define GL_DEPTH_ATTACHMENT		0x8D00
#define GL_FRAMEBUFFER_COMPLETE	0x8CD5
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER		0x8B30
#define GL_VERTEX_SHADER		0x8B31
#define GL_COMPILE_STATUS		0x8B81
#define GL_LINK_STATUS			0x8B82
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24	0x81A6
#endif

static qboolean	modernReady;		// les entrees GL sont la et les tampons crees
static qboolean	modernFailed;		// on a essaye et ca n'a pas marche : ne pas reessayer
static int		modernWidth;
static int		modernHeight;
static GLuint	modernSceneTex;		// l'image finie de la scene
static GLuint	modernDepthTex;		// sa profondeur
static GLuint	modernComposite;	// scene + occlusion -> ecran
static GLuint	modernAoProg;		// calcul de l'occlusion
static GLuint	modernAoTex;		// son resultat, une seule composante
static GLuint	modernAoFbo;
// --- la carte d'ombre du soleil (profondeur du monde vue depuis le soleil)
static GLuint	modernSunTex;
static GLuint	modernSunFbo;
static int		modernSunSize = 2048;
static vec3_t	modernSunCenter;	// autour de quoi elle a ete construite
static float	modernSunRange;
static qboolean	modernSunValid;
static float	modernSunMatrix[16];	// monde -> carte d'ombre (0..1)

/*
===============
R_ModernLoadEntryPoints

Rend qtrue si tout ce dont la couche a besoin est disponible. Un seul manquant
et on renonce : mieux vaut le rendu d'origine qu'un ecran noir.
===============
*/
#define GRAB( var, name ) \
	var = (decltype( var ))ri.GL_GetProcAddress( name ); \
	if ( !var ) { missing = name; }

static qboolean R_ModernLoadEntryPoints( void )
{
	const char *missing = NULL;

	GRAB( p_glGenFramebuffers,			"glGenFramebuffers" );
	GRAB( p_glDeleteFramebuffers,		"glDeleteFramebuffers" );
	GRAB( p_glBindFramebuffer,			"glBindFramebuffer" );
	GRAB( p_glFramebufferTexture2D,		"glFramebufferTexture2D" );
	GRAB( p_glCheckFramebufferStatus,	"glCheckFramebufferStatus" );
	GRAB( p_glCreateShader,				"glCreateShader" );
	GRAB( p_glShaderSource,				"glShaderSource" );
	GRAB( p_glCompileShader,			"glCompileShader" );
	GRAB( p_glGetShaderiv,				"glGetShaderiv" );
	GRAB( p_glGetShaderInfoLog,			"glGetShaderInfoLog" );
	GRAB( p_glDeleteShader,				"glDeleteShader" );
	GRAB( p_glCreateProgram,			"glCreateProgram" );
	GRAB( p_glAttachShader,				"glAttachShader" );
	GRAB( p_glLinkProgram,				"glLinkProgram" );
	GRAB( p_glGetProgramiv,				"glGetProgramiv" );
	GRAB( p_glGetProgramInfoLog,		"glGetProgramInfoLog" );
	GRAB( p_glDeleteProgram,			"glDeleteProgram" );
	GRAB( p_glUseProgram,				"glUseProgram" );
	GRAB( p_glGetUniformLocation,		"glGetUniformLocation" );
	GRAB( p_glUniform1i,				"glUniform1i" );
	GRAB( p_glUniform1f,				"glUniform1f" );
	GRAB( p_glUniform2f,				"glUniform2f" );
	GRAB( p_glUniform3f,				"glUniform3f" );
	GRAB( p_glUniform4f,				"glUniform4f" );
	GRAB( p_glUniformMatrix4fv,			"glUniformMatrix4fv" );

	if ( missing )
	{
		ri.Printf( PRINT_ALL, "...rendu moderne indisponible : %s manque\n", missing );
		return qfalse;
	}
	return qtrue;
}

/*
===============
R_ModernCompile

Un programme GLSL depuis deux sources. Le journal du pilote est recopie tel
quel dans la console : c'est la seule chose qui dit pourquoi un shader refuse.
===============
*/
static GLuint R_ModernCompileStage( GLenum type, const char *src, const char *what )
{
	const GLuint	obj = p_glCreateShader( type );
	GLint			ok = 0;

	p_glShaderSource( obj, 1, &src, NULL );
	p_glCompileShader( obj );
	p_glGetShaderiv( obj, GL_COMPILE_STATUS, &ok );
	if ( !ok )
	{
		char log[2048] = { 0 };

		p_glGetShaderInfoLog( obj, sizeof( log ) - 1, NULL, log );
		ri.Printf( PRINT_ALL, "rendu moderne : %s refuse\n%s\n", what, log );
		p_glDeleteShader( obj );
		return 0;
	}
	return obj;
}

GLuint R_ModernCompile( const char *vertex, const char *fragment, const char *what )
{
	const GLuint	vs = R_ModernCompileStage( GL_VERTEX_SHADER, vertex, va( "%s (sommets)", what ) );
	const GLuint	fs = vs ? R_ModernCompileStage( GL_FRAGMENT_SHADER, fragment, va( "%s (pixels)", what ) ) : 0;
	GLuint			prog;
	GLint			ok = 0;

	if ( !vs || !fs )
	{
		if ( vs ) p_glDeleteShader( vs );
		if ( fs ) p_glDeleteShader( fs );
		return 0;
	}
	prog = p_glCreateProgram();
	p_glAttachShader( prog, vs );
	p_glAttachShader( prog, fs );
	p_glLinkProgram( prog );
	p_glGetProgramiv( prog, GL_LINK_STATUS, &ok );
	p_glDeleteShader( vs );
	p_glDeleteShader( fs );
	if ( !ok )
	{
		char log[2048] = { 0 };

		p_glGetProgramInfoLog( prog, sizeof( log ) - 1, NULL, log );
		ri.Printf( PRINT_ALL, "rendu moderne : %s ne se lie pas\n%s\n", what, log );
		p_glDeleteProgram( prog );
		return 0;
	}
	return prog;
}

// Etape 0 : on rend l'image telle quelle. Si la capture rend autre chose que
// l'original, c'est visible tout de suite - et il n'y a encore aucun effet
// pour masquer une erreur de plomberie.
static const char *vsPassthrough =
	"#version 120\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"	uv = gl_MultiTexCoord0.xy;\n"
	"	gl_Position = gl_Vertex;\n"
	"}\n";

// Reconstruire la position d'un pixel dans l'espace de la camera a partir de sa
// profondeur. La matrice de projection du moteur n'a que six termes utiles, et
// son inverse s'ecrit a la main - inutile d'inverser une matrice 4x4 :
//
//   [ A 0 C 0 ]              [ 1/A  0   0   C/A ]
//   [ 0 B D 0 ]   inverse =  [  0  1/B  0   D/B ]
//   [ 0 0 E F ]              [  0   0   0   -1  ]
//   [ 0 0 -1 0 ]             [  0   0  1/F  E/F ]
//
// Partage par l'occlusion, et demain par les ombres et la volumetrique.
static const char *glslViewPos =
	"uniform vec2 projAB;\n"		// A, B
	"uniform vec2 projCD;\n"		// C, D
	"uniform vec2 projEF;\n"		// E, F
	"uniform sampler2D sceneDepth;\n"
	"vec3 viewPosAt( vec2 t, float d ) {\n"
	"	vec3 n = vec3( t * 2.0 - 1.0, d * 2.0 - 1.0 );\n"
	"	float w = n.z / projEF.y + projEF.x / projEF.y;\n"
	"	vec3 p = vec3( ( n.x + projCD.x ) / projAB.x,\n"
	"	               ( n.y + projCD.y ) / projAB.y,\n"
	"	               -1.0 );\n"
	"	return p / w;\n"
	"}\n"
	"vec3 viewPos( vec2 t ) { return viewPosAt( t, texture2D( sceneDepth, t ).r ); }\n";

static const char *fsAo =
	"#version 120\n"
	"varying vec2 uv;\n"
	"uniform vec2 texel;\n"			// 1 / taille
	"uniform float radius;\n"		// en unites du jeu
	"uniform float bias;\n"
	"uniform vec3 sunDir;\n"		// vers le soleil, en espace camera
	"uniform float sunOn;\n"
	"uniform float sunLength;\n"	// portee de l'ombre, en unites du jeu
	"uniform int dbg;\n"
	"const int SUN_STEPS = 24;\n"
	"%s"
	"%s"							// viewPos
	// La normale vient des variations de profondeur. Une difference centree
	// deborde sur les silhouettes ; on garde donc, de chaque cote, le voisin le
	// plus proche en profondeur - c'est celui qui appartient encore a la meme
	// surface.
	"vec3 normalAt( vec2 t, vec3 p ) {\n"
	"	vec3 l = viewPos( t - vec2( texel.x, 0.0 ) ) - p;\n"
	"	vec3 r = viewPos( t + vec2( texel.x, 0.0 ) ) - p;\n"
	"	vec3 d = viewPos( t - vec2( 0.0, texel.y ) ) - p;\n"
	"	vec3 u = viewPos( t + vec2( 0.0, texel.y ) ) - p;\n"
	"	vec3 h = ( abs( l.z ) < abs( r.z ) ) ? -l : r;\n"
	"	vec3 v = ( abs( d.z ) < abs( u.z ) ) ? -d : u;\n"
	"	return normalize( cross( h, v ) );\n"
	"}\n"
	"float hash( vec2 c ) { return fract( sin( dot( c, vec2( 12.9898, 78.233 ) ) ) * 43758.5453 ); }\n"
	"void main() {\n"
	"	float d = texture2D( sceneDepth, uv ).r;\n"
	"	if ( d >= 1.0 ) { gl_FragColor = vec4( 1.0 ); return; }\n"	// le ciel
	"	vec3 p = viewPosAt( uv, d );\n"
	"	vec3 n = normalAt( uv, p );\n"
	"	float ang = hash( gl_FragCoord.xy ) * 6.2831853;\n"
	"	float occ = 0.0;\n"
	"	const int STEPS = 12;\n"
	"	for ( int i = 0; i < STEPS; i++ ) {\n"
	// spirale : l'angle tourne, le rayon croit - une repartition reguliere sans
	// avoir a transporter une table d'echantillons
	"		float f = ( float( i ) + 0.5 ) / float( STEPS );\n"
	"		float a = ang + f * 6.2831853 * 2.4;\n"
	"		vec3 dir = vec3( cos( a ), sin( a ), 0.0 );\n"
	"		dir = normalize( dir + n * 0.9 );\n"				// hemisphere autour de la normale
	"		vec3 sp = p + dir * radius * sqrt( f );\n"
	// reprojeter l'echantillon a l'ecran
	"		vec2 st = vec2( sp.x / -sp.z * projAB.x - projCD.x,\n"
	"		                sp.y / -sp.z * projAB.y - projCD.y ) * 0.5 + 0.5;\n"
	"		if ( st.x < 0.0 || st.x > 1.0 || st.y < 0.0 || st.y > 1.0 ) continue;\n"
	"		vec3 q = viewPos( st );\n"
	"		vec3 dv = q - p;\n"
	"		float dist = length( dv );\n"
	"		if ( dist < 0.0001 ) continue;\n"
	"		float cosa = dot( n, dv / dist );\n"
	// une surface lointaine ne doit pas occulter : sinon un mur au fond
	// assombrit tout ce qui passe devant lui
	"		float fall = clamp( 1.0 - ( dist - radius ) / radius, 0.0, 1.0 );\n"
	"		occ += clamp( cosa - bias, 0.0, 1.0 ) * fall;\n"
	"	}\n"
	"	occ = occ / float( STEPS );\n"
	// --- ombre du soleil, dans le meme tampon (canal vert)
	"	float shade = 1.0;\n"
	"	if ( sunOn > 0.5 ) {\n"
	"		float ndl = dot( n, sunDir );\n"
	// La carte d'ombre d'abord : elle voit aussi ce qui est hors du cadre, donc
	// l'ombre ne change plus quand on tourne la tete. Elle rend -1 la ou elle
	// n'a rien a dire, et on retombe alors sur la marche en espace ecran.
	"		float fromMap = sunShadowAt( p, ndl );\n"
	"		if ( fromMap >= 0.0 ) {\n"
	"			shade = ( ndl > 0.05 ) ? fromMap : 1.0;\n"
	"		} else\n"
	// Une surface qui tourne le dos au soleil est deja sombre dans la carte de
	// lumiere du jeu : la reassombrir la peindrait deux fois.
	"		if ( ndl > 0.05 ) {\n"
	"			float stepLen = sunLength / float( SUN_STEPS );\n"
	"			vec3 sp = p + n * 1.5;\n"		// decoller de la surface
	"			float jit = hash( gl_FragCoord.yx );\n"
	"			for ( int k = 0; k < SUN_STEPS; k++ ) {\n"
	"				sp += sunDir * stepLen * ( 0.75 + 0.5 * jit );\n"
	"				vec2 st = vec2( sp.x / -sp.z * projAB.x - projCD.x,\n"
	"				                sp.y / -sp.z * projAB.y - projCD.y ) * 0.5 + 0.5;\n"
	"				if ( st.x < 0.0 || st.x > 1.0 || st.y < 0.0 || st.y > 1.0 ) break;\n"
	"				float sceneZ = viewPos( st ).z;\n"
	// en espace camera z est negatif vers le fond : la scene est DEVANT le
	// point marche quand son z est plus grand
	"				float diff = sceneZ - sp.z;\n"
	"				if ( diff > 1.0 && diff < 120.0 ) { shade = 0.0; break; }\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	if ( dbg == 7 ) {\n"
	"		float ndl = dot( n, sunDir );\n"
	"		float fm = sunShadowAt( p, ndl );\n"
	"		gl_FragColor = vec4( fm < 0.0 ? 1.0 : 0.0, fm < 0.0 ? 0.0 : fm, ndl > 0.05 ? 1.0 : 0.0, 1.0 );\n"
	"		return;\n"
	"	}\n"
	"	if ( dbg == 8 || dbg == 9 ) {\n"
	"		vec4 lp = viewToSun * vec4( p, 1.0 );\n"
	"		float dm = texture2D( sunMap, lp.xy ).r;\n"
	"		gl_FragColor = ( dbg == 8 ) ? vec4( lp.xyz, 1.0 ) : vec4( lp.z, dm, abs( lp.z - dm ) * 20.0, 1.0 );\n"
	"		return;\n"
	"	}\n"
	"	gl_FragColor = vec4( clamp( 1.0 - occ, 0.0, 1.0 ), shade, 0.0, 1.0 );\n"
	"}\n";

// Composition : l'occlusion est bruitee par construction (un angle aleatoire
// par pixel), on l'adoucit ici au lieu d'y consacrer une passe de plus.
// Lecture de la carte d'ombre. On remonte du pixel a sa position dans le
// MONDE (la matrice fournie contient deja l'inverse de la vue), puis on
// regarde ce que le soleil voyait la-bas.
static const char *glslSunMap =
	"uniform sampler2D sunMap;\n"
	"uniform mat4 viewToSun;\n"
	"uniform float sunMapOn;\n"
	"uniform float sunTexel;\n"
	"float sunShadowAt( vec3 viewP, float ndl ) {\n"
	"	if ( sunMapOn < 0.5 ) return -1.0;\n"
	"	vec4 lp = viewToSun * vec4( viewP, 1.0 );\n"
	"	if ( lp.x < 0.01 || lp.x > 0.99 || lp.y < 0.01 || lp.y > 0.99 || lp.z > 1.0 ) return -1.0;\n"
	// Le biais suit l'inclinaison : une surface rasante demande plus de marge,
	// sinon elle s'ombre elle-meme en bandes.
	"	float bias = 0.0012 + 0.006 * ( 1.0 - clamp( ndl, 0.0, 1.0 ) );\n"
	"	float lit = 0.0;\n"
	"	for ( int y = -1; y <= 1; y++ )\n"
	"		for ( int x = -1; x <= 1; x++ ) {\n"
	"			float d = texture2D( sunMap, lp.xy + vec2( float( x ), float( y ) ) * sunTexel ).r;\n"
	"			lit += ( lp.z - bias <= d ) ? 1.0 : 0.0;\n"
	"		}\n"
	"	return lit / 9.0;\n"
	"}\n";

static const char *fsComposite =
	"#version 120\n"
	"uniform sampler2D scene;\n"
	"uniform sampler2D ao;\n"
	"uniform vec2 texel;\n"
	"uniform float intensity;\n"
	"uniform float sunStrength;\n"
	"uniform vec2 sunUV;\n"			// le soleil a l'ecran
	"uniform float raysOn;\n"
	"uniform vec3 raysColor;\n"
	"uniform int debugMode;\n"
	"uniform sampler2D rtLight;\n"		// le ray tracing : lumiere ajoutee
	"uniform sampler2D rtRefl;\n"		// ... et reflet (rgb) avec sa force (a)
	"uniform float rtOn;\n"
	"uniform sampler2D rtMat;\n"		// le tampon de materiaux : normale de relief (vue), genre+brillance
	"uniform sampler2D rtMatDepth;\n"
	"uniform sampler2D rtMatGeo;\n"
	"uniform vec3 sunView;\n"
	"uniform float reliefOn;\n"
	"%s"
	"varying vec2 uv;\n"
	"float hash( vec2 c ) { return fract( sin( dot( c, vec2( 12.9898, 78.233 ) ) ) * 43758.5453 ); }\n"
	"vec3 normalAt( vec2 t, vec3 p ) {\n"
	"	vec3 l = viewPos( t - vec2( texel.x, 0.0 ) ) - p;\n"
	"	vec3 r = viewPos( t + vec2( texel.x, 0.0 ) ) - p;\n"
	"	vec3 d = viewPos( t - vec2( 0.0, texel.y ) ) - p;\n"
	"	vec3 u = viewPos( t + vec2( 0.0, texel.y ) ) - p;\n"
	"	vec3 h = ( abs( l.z ) < abs( r.z ) ) ? -l : r;\n"
	"	vec3 v = ( abs( d.z ) < abs( u.z ) ) ? -d : u;\n"
	"	return normalize( cross( h, v ) );\n"
	"}\n"
	"void main() {\n"
	"	vec4 c = texture2D( scene, uv );\n"
	"	float a = 0.0;\n"
	"	for ( int y = -2; y <= 2; y++ )\n"
	"		for ( int x = -2; x <= 2; x++ )\n"
	"			a += texture2D( ao, uv + vec2( float( x ), float( y ) ) * texel ).r;\n"
	"	a = a / 25.0;\n"
	"	a = mix( 1.0, a, intensity );\n"
	"	float sh = 0.0;\n"
	"	for ( int y = -2; y <= 2; y++ )\n"
	"		for ( int x = -2; x <= 2; x++ )\n"
	"			sh += texture2D( ao, uv + vec2( float( x ), float( y ) ) * texel ).g;\n"
	// Un pixel deja sombre n'est presque plus assombri : la carte de lumiere
	// du jeu a deja fait son travail en interieur, et une coursive entiere
	// virait au noir (capture de JD, 24/09).
	"	float lum = dot( c.rgb, vec3( 0.3, 0.59, 0.11 ) );\n"
	"	sh = mix( 1.0, sh / 25.0, sunStrength * smoothstep( 0.06, 0.30, lum ) );\n"
	// --- rayons crepusculaires : compter le ciel entre ce pixel et le soleil
	"	vec3 rays = vec3( 0.0 );\n"
	"	if ( raysOn > 0.0 ) {\n"
	"		const int RSTEPS = 24;\n"
	"		vec2 delta = sunUV - uv;\n"
	"		float dist = length( delta );\n"
	"		if ( dist > 0.0005 ) {\n"
	"			vec2 dir = delta / dist;\n"
	// on ne remonte pas jusqu'au soleil : au-dela d'une certaine longueur le
	// rai perd tout sens et devient une trainee grise
	"			float len = min( dist, 0.55 );\n"
	"			float jit = hash( gl_FragCoord.xy );\n"
	"			float sky = 0.0;\n"
	"			for ( int i = 0; i < RSTEPS; i++ ) {\n"
	"				float f = ( float( i ) + jit ) / float( RSTEPS );\n"
	"				vec2 st = uv + dir * len * f;\n"
	"				if ( st.x < 0.0 || st.x > 1.0 || st.y < 0.0 || st.y > 1.0 ) break;\n"
	"				if ( texture2D( sceneDepth, st ).r >= 0.9999 ) sky += 1.0 - f;\n"
	"			}\n"
	"			sky /= float( RSTEPS ) * 0.5;\n"
	// plus on est loin du soleil, plus le rai s'eteint
	"			float att = 1.0 / ( 1.0 + dist * 3.0 );\n"
	"			rays = raysColor * sky * att * raysOn;\n"
	"		}\n"
	"	}\n"
	"	if ( debugMode == 1 ) {\n"
	"		float d = texture2D( sceneDepth, uv ).r;\n"
	"		gl_FragColor = vec4( vec3( 1.0 - pow( d, 64.0 ) ), 1.0 );\n"
	"	} else if ( debugMode == 3 ) {\n"
	"		gl_FragColor = vec4( vec3( a ), 1.0 );\n"		// l'occlusion seule
	"	} else if ( debugMode == 2 && uv.x > 0.5 ) {\n"
	"		gl_FragColor = vec4( c.rgb * vec3( 1.0, 0.45, 0.25 ), c.a );\n"
	"	} else if ( debugMode == 4 ) {\n"
	"		gl_FragColor = vec4( vec3( sh ), 1.0 );\n"		// l'ombre seule
	"	} else if ( debugMode >= 7 && debugMode <= 9 ) {\n"
	"		gl_FragColor = vec4( texture2D( ao, uv ).rgb, 1.0 );\n"
	"	} else if ( debugMode == 10 ) {\n"
	"		gl_FragColor = vec4( texture2D( rtLight, uv ).rgb, 1.0 );\n"	// la lumiere tracee seule
	"	} else if ( debugMode == 12 ) {\n"
	"		gl_FragColor = vec4( texture2D( ao, uv ).rgb, 1.0 );\n"	// la cible d occlusion brute
	"	} else if ( debugMode == 11 ) {\n"
	"		vec4 R = texture2D( rtRefl, uv );\n"
	"		gl_FragColor = vec4( R.rgb * R.a, 1.0 );\n"			// le reflet seul
	"	} else if ( debugMode == 5 ) {\n"
	"		gl_FragColor = vec4( rays, 1.0 );\n"			// les rayons seuls
	"	} else {\n"
	"		vec3 col = c.rgb * a * sh;\n"
	"		if ( rtOn > 0.5 ) {\n"
	"			vec4 L = texture2D( rtLight, uv );\n"
	"			vec4 R = texture2D( rtRefl, uv );\n"
	"			col += L.rgb * ( c.rgb * 0.7 + 0.15 );\n"
	"			col = mix( col, R.rgb, R.a );\n"
	// Le relief du decor : la normale de la texture contre la normale
	// geometrique, face au soleil la ou il eclaire (le G brut, sans le
	// flou), et un leger modele partout (les creux tournent le dos a la
	// camera).
	"			if ( reliefOn > 0.5 ) {\n"
	"				vec4 mm = texture2D( rtMat, uv );\n"
	"				float dd = texture2D( sceneDepth, uv ).r;\n"
	"				float mdd = texture2D( rtMatDepth, uv ).r;\n"
	"				if ( mm.a > 0.0 && abs( floor( mm.a * 5.0 + 0.001 ) - 1.0 ) < 0.5 && abs( mdd - dd ) < 0.0004 && dd < 1.0 ) {\n"
	"					vec3 pv = viewPosAt( uv, dd );\n"
	"					vec3 ng = normalize( texture2D( rtMatGeo, uv ).xyz * 2.0 - 1.0 );\n"
	"					vec3 nd = normalize( mm.xyz * 2.0 - 1.0 );\n"
	"					float lit = texture2D( ao, uv ).g * sunStrength;\n"
	"					float gl = dot( ng, sunView );\n"
	"					float rel = ( gl > 0.05 ) ? clamp( dot( nd, sunView ) / gl, 0.3, 1.7 ) : 1.0;\n"
	"					float amb = 1.0 + 0.35 * ( nd.z - ng.z ) * reliefOn;\n"
	"					col *= mix( amb, rel * amb, lit * step( 0.05, gl ) );\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"		gl_FragColor = vec4( col + rays, c.a );\n"
	"	}\n"
	"}\n";

/*
===============
R_ModernCreateTargets

Les textures de capture suivent la taille de la fenetre. Non puissance de deux :
GL_ARB_texture_non_power_of_two est acquis partout ou GLSL l'est.
===============
*/
// Vide le drapeau d'erreur : on s'initialise au milieu d'une image, ce qui
// traine peut etre deja une erreur venue du rendu d'origine, et elle n'est pas
// la notre.
void R_ModernDrainErrors( void )
{
	for ( int i = 0; i < 16 && qglGetError() != GL_NO_ERROR; i++ ) {}
}

qboolean R_ModernStep( const char *what )
{
	const GLenum err = qglGetError();

	if ( err != GL_NO_ERROR )
	{
		ri.Printf( PRINT_ALL, "...rendu moderne : %s -> erreur GL 0x%x\n", what, err );
		return qfalse;
	}
	return qtrue;
}

static qboolean R_ModernCreateTargets( int width, int height )
{
	R_ModernDrainErrors();
	if ( modernSceneTex )
	{
		qglDeleteTextures( 1, &modernSceneTex );
		qglDeleteTextures( 1, &modernDepthTex );
		qglDeleteTextures( 1, &modernAoTex );
		modernSceneTex = modernDepthTex = modernAoTex = 0;
	}

	qglGenTextures( 1, &modernSceneTex );
	qglBindTexture( GL_TEXTURE_2D, modernSceneTex );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	if ( !R_ModernStep( "texture de couleur" ) )
	{
		return qfalse;
	}

	qglGenTextures( 1, &modernDepthTex );
	qglBindTexture( GL_TEXTURE_2D, modernDepthTex );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	// L'occlusion vit dans sa propre cible : elle est calculee en une passe,
	// puis adoucie a la composition.
	qglGenTextures( 1, &modernAoTex );
	qglBindTexture( GL_TEXTURE_2D, modernAoTex );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	qglBindTexture( GL_TEXTURE_2D, 0 );

	if ( !modernAoFbo )
	{
		p_glGenFramebuffers( 1, &modernAoFbo );
	}
	p_glBindFramebuffer( GL_FRAMEBUFFER, modernAoFbo );
	p_glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, modernAoTex, 0 );
	if ( p_glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		ri.Printf( PRINT_ALL, "...rendu moderne : cible d'occlusion incomplete\n" );
		p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return qfalse;
	}
	p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	if ( !R_ModernStep( va( "textures %ix%i", width, height ) ) )
	{
		return qfalse;
	}
	modernWidth = width;
	modernHeight = height;
	return qtrue;
}

/*
===============
R_ModernInit / R_ModernShutdown
===============
*/
void R_ModernInit( void )
{
	modernReady = qfalse;
	if ( modernFailed )
	{
		return;		// deja essaye, deja rate : ne pas rejouer l'echec chaque image
	}
	ri.Printf( PRINT_ALL, "----- rendu moderne -----\n" );
	ri.Printf( PRINT_ALL, "...GL %s\n", glConfig.version_string ? glConfig.version_string : "?" );
	if ( !R_ModernLoadEntryPoints() )
	{
		modernFailed = qtrue;
		return;
	}
	if ( !R_ModernCreateTargets( glConfig.vidWidth, glConfig.vidHeight ) )
	{
		modernFailed = qtrue;
		return;
	}
	modernAoProg = R_ModernCompile( vsPassthrough, va( fsAo, glslViewPos, glslSunMap ), "occlusion ambiante" );
	modernComposite = R_ModernCompile( vsPassthrough, va( fsComposite, glslViewPos ), "composition" );
	if ( !modernAoProg || !modernComposite )
	{
		modernFailed = qtrue;
		return;
	}
	modernReady = qtrue;
	ri.Printf( PRINT_ALL, "...actif en %ix%i\n", modernWidth, modernHeight );
}

void R_ModernShutdown( void )
{
	R_ModernRTShutdown();
	if ( modernComposite )
	{
		p_glDeleteProgram( modernComposite );
		modernComposite = 0;
	}
	if ( modernAoProg )
	{
		p_glDeleteProgram( modernAoProg );
		modernAoProg = 0;
	}
	if ( modernAoFbo )
	{
		p_glDeleteFramebuffers( 1, &modernAoFbo );
		modernAoFbo = 0;
	}
	if ( modernSunFbo )
	{
		p_glDeleteFramebuffers( 1, &modernSunFbo );
		modernSunFbo = 0;
	}
	if ( modernSunTex )
	{
		qglDeleteTextures( 1, &modernSunTex );
		modernSunTex = 0;
	}
	modernSunValid = qfalse;
	if ( modernSceneTex )
	{
		qglDeleteTextures( 1, &modernSceneTex );
		qglDeleteTextures( 1, &modernDepthTex );
		qglDeleteTextures( 1, &modernAoTex );
		modernSceneTex = modernDepthTex = modernAoTex = 0;
	}
	modernReady = qfalse;
}


/*
===============
La carte d'ombre du soleil

Le decor ne bouge pas et le soleil non plus : la carte n'est refaite que quand
le joueur s'est assez eloigne du centre autour duquel elle a ete construite.
On la remplit en parcourant nous-memes les surfaces du BSP - c'est ce qui permet
de ne rien changer a la facon dont le moteur dessine.
===============
*/
static void R_ModernMulMat( const float *a, const float *b, float *out )
{	// out = a * b, colonnes majeures comme OpenGL
	for ( int c = 0; c < 4; c++ )
	{
		for ( int r = 0; r < 4; r++ )
		{
			out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0]
				+ a[1 * 4 + r] * b[c * 4 + 1]
				+ a[2 * 4 + r] * b[c * 4 + 2]
				+ a[3 * 4 + r] * b[c * 4 + 3];
		}
	}
}

// L'inverse d'une matrice de vue (rotation + translation) : la rotation se
// transpose, la translation se ramene dans le nouveau repere.
static void R_ModernInvertView( const float *m, float *out )
{
	for ( int r = 0; r < 3; r++ )
	{
		for ( int c = 0; c < 3; c++ )
		{
			out[c * 4 + r] = m[r * 4 + c];
		}
	}
	for ( int r = 0; r < 3; r++ )
	{
		// la translation se ramene par la rotation TRANSPOSEE : c'est m[r*4+k]
		// qu'il faut lire, pas m[k*4+r] - l'inverse rendait sinon un point qui
		// n'avait rien a voir avec la camera (mesure : 1093 -2634 -1522 au lieu
		// de 952 -3081 211)
		out[12 + r] = -( m[12] * m[r * 4 + 0] + m[13] * m[r * 4 + 1] + m[14] * m[r * 4 + 2] );
	}
	out[3] = out[7] = out[11] = 0.0f;
	out[15] = 1.0f;
}

static qboolean R_ModernSunTargets( void )
{
	if ( modernSunTex )
	{
		return qtrue;
	}
	R_ModernDrainErrors();
	qglGenTextures( 1, &modernSunTex );
	qglBindTexture( GL_TEXTURE_2D, modernSunTex );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, modernSunSize, modernSunSize, 0,
		GL_DEPTH_COMPONENT, GL_FLOAT, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	// hors de la carte, la profondeur maximale = "rien ne bouche le soleil"
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	qglBindTexture( GL_TEXTURE_2D, 0 );

	p_glGenFramebuffers( 1, &modernSunFbo );
	p_glBindFramebuffer( GL_FRAMEBUFFER, modernSunFbo );
	p_glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, modernSunTex, 0 );
	qglDrawBuffer( GL_NONE );
	qglReadBuffer( GL_NONE );
	const qboolean ok = (qboolean)( p_glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE );
	p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	if ( !ok )
	{
		ri.Printf( PRINT_ALL, "...rendu moderne : carte d'ombre incomplete\n" );
		return qfalse;
	}
	return R_ModernStep( "carte d'ombre" );
}

// Une surface du BSP, en profondeur seule. Pas de texture, pas de couleur : on
// ne veut que la distance au soleil.
static void R_ModernDrawWorldSurface( const msurface_t *surf )
{
	if ( !surf->data || !surf->shader )
	{
		return;
	}
	if ( surf->shader->sort > SS_OPAQUE || ( surf->shader->surfaceFlags & ( SURF_NODRAW | SURF_SKY ) ) )
	{
		return;		// le ciel ne fait pas d'ombre, le transparent non plus
	}
	if ( *surf->data == SF_FACE )
	{
		const srfSurfaceFace_t *face = (const srfSurfaceFace_t *)surf->data;

		if ( face->numPoints < 3 || face->numIndices < 3 )
		{
			return;
		}
		qglVertexPointer( 3, GL_FLOAT, VERTEXSIZE * sizeof( float ), face->points[0] );
		qglDrawElements( GL_TRIANGLES, face->numIndices, GL_UNSIGNED_INT,
			( (const byte *)face ) + face->ofsIndices );
	}
	else if ( *surf->data == SF_TRIANGLES )
	{
		const srfTriangles_t *tri = (const srfTriangles_t *)surf->data;

		if ( tri->numVerts < 3 || tri->numIndexes < 3 )
		{
			return;
		}
		qglVertexPointer( 3, GL_FLOAT, sizeof( drawVert_t ), tri->verts->xyz );
		qglDrawElements( GL_TRIANGLES, tri->numIndexes, GL_UNSIGNED_INT, tri->indexes );
	}
	else if ( *surf->data == SF_GRID )
	{	// une surface courbe : une grille de sommets sans indices tout prets,
		// on les fabrique (deux triangles par case)
		static unsigned int	*gridIdx;
		static int			gridIdxSize;
		const srfGridMesh_t	*grid = (const srfGridMesh_t *)surf->data;
		const int			w = grid->width, h = grid->height;
		const int			need = ( w - 1 ) * ( h - 1 ) * 6;

		if ( w < 2 || h < 2 )
		{
			return;
		}
		if ( need > gridIdxSize )
		{
			free( gridIdx );
			gridIdx = (unsigned int *)malloc( (size_t)need * sizeof( unsigned int ) );
			gridIdxSize = gridIdx ? need : 0;
		}
		if ( !gridIdx )
		{
			return;
		}
		unsigned int *o = gridIdx;
		for ( int r = 0; r < h - 1; r++ )
		{
			for ( int c = 0; c < w - 1; c++ )
			{
				const unsigned int a = r * w + c;

				*o++ = a;		*o++ = a + 1;		*o++ = a + w;
				*o++ = a + 1;	*o++ = a + w + 1;	*o++ = a + w;
			}
		}
		qglVertexPointer( 3, GL_FLOAT, sizeof( drawVert_t ), grid->verts->xyz );
		qglDrawElements( GL_TRIANGLES, need, GL_UNSIGNED_INT, gridIdx );
	}
}

static void R_ModernBuildSunMap( const vec3_t sunWorld, const vec3_t center, float range )
{
	vec3_t	dir, right, up, eye;
	float	view[16], proj[16], lightMat[16], bias[16];

	if ( !tr.world || !R_ModernSunTargets() )
	{
		modernSunValid = qfalse;
		return;
	}

	// un repere autour de la direction du soleil
	VectorCopy( sunWorld, dir );
	VectorNormalize( dir );
	// un repere orthonorme construit a la main : la fonction du jeu n'est pas
	// declaree cote renderer
	vec3_t	seed;
	VectorSet( seed, 0.0f, 0.0f, 1.0f );
	if ( fabs( dir[2] ) > 0.9f )
	{
		VectorSet( seed, 1.0f, 0.0f, 0.0f );
	}
	CrossProduct( seed, dir, right );
	VectorNormalize( right );
	CrossProduct( dir, right, up );
	VectorNormalize( up );
	VectorMA( center, range * 2.0f, dir, eye );		// on recule "vers le soleil"

	// matrice de vue depuis le soleil : on regarde vers -dir
	view[0] = right[0];	view[4] = right[1];	view[8]  = right[2];	view[12] = -DotProduct( right, eye );
	view[1] = up[0];	view[5] = up[1];	view[9]  = up[2];		view[13] = -DotProduct( up, eye );
	view[2] = dir[0];	view[6] = dir[1];	view[10] = dir[2];		view[14] = -DotProduct( dir, eye );
	view[3] = 0;		view[7] = 0;		view[11] = 0;			view[15] = 1;

	// projection orthographique : le soleil est infiniment loin
	const float zNear = 1.0f;
	const float zFar = range * 6.0f;
	memset( proj, 0, sizeof( proj ) );
	proj[0]  = 1.0f / range;
	proj[5]  = 1.0f / range;
	proj[10] = -2.0f / ( zFar - zNear );
	proj[14] = -( zFar + zNear ) / ( zFar - zNear );
	proj[15] = 1.0f;

	R_ModernMulMat( proj, view, lightMat );

	// de l'espace de coupe (-1..1) aux coordonnees de texture (0..1)
	memset( bias, 0, sizeof( bias ) );
	bias[0] = bias[5] = bias[10] = 0.5f;
	bias[12] = bias[13] = bias[14] = 0.5f;
	bias[15] = 1.0f;
	R_ModernMulMat( bias, lightMat, modernSunMatrix );

	// --- la passe de profondeur
	p_glBindFramebuffer( GL_FRAMEBUFFER, modernSunFbo );
	qglViewport( 0, 0, modernSunSize, modernSunSize );
	qglScissor( 0, 0, modernSunSize, modernSunSize );
	// Poser TOUT l'etat de profondeur : le moteur laisse le sien derriere lui et
	// on ne sait pas lequel. Mesure a l'appui : la carte sortait entierement a
	// 0.0000, parce qu'elle etait effacee avec 0 au lieu de 1 - rien ne peut
	// alors etre plus proche, donc aucune ombre.
	qglEnable( GL_DEPTH_TEST );
	qglDepthMask( GL_TRUE );
	qglDepthFunc( GL_LEQUAL );
	qglClearDepth( 1.0 );
	qglClear( GL_DEPTH_BUFFER_BIT );

	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadMatrixf( proj );
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadMatrixf( view );

	GL_State( GLS_DEFAULT );
	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	qglDisable( GL_TEXTURE_2D );
	// Aucun culling : les murs du BSP n'ont qu'une face (l'autre donne sur le
	// vide), elle doit ecrire sa profondeur quel que soit son sens. Le biais
	// de lecture absorbe l'acne. GL_Cull, et non glCullFace : le moteur suit
	// cet etat dans glState et ne le reemet que s'il croit qu'il a change.
	GL_Cull( CT_TWO_SIDED );
	// GL_VERTEX_ARRAY reste ACTIF : le moteur ne l'active qu'a l'initialisation
	// et dessine tout par glDrawElements ; le couper ici figeait toute l'image.
	qglEnableClientState( GL_VERTEX_ARRAY );
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	qglDisableClientState( GL_COLOR_ARRAY );

	const float	lo[3] = { center[0] - range * 1.5f, center[1] - range * 1.5f, center[2] - range * 1.5f };
	const float	hi[3] = { center[0] + range * 1.5f, center[1] + range * 1.5f, center[2] + range * 1.5f };
	int			drawn = 0;

	// Le monde seulement (bmodels[0]) : les sous-modeles - portes, ascenseurs,
	// le train de t1_rail - sont dans le meme tableau mais a leur position
	// d'origine, ils projetteraient une ombre la ou ils ne sont plus.
	const bmodel_t	*world = &tr.world->bmodels[0];

	for ( int i = 0; i < world->numSurfaces; i++ )
	{
		const msurface_t *surf = &world->firstSurface[i];

		if ( *surf->data == SF_TRIANGLES )
		{	// ces surfaces portent leurs limites : on peut ecarter de loin
			const srfTriangles_t *tri = (const srfTriangles_t *)surf->data;

			if ( tri->bounds[1][0] < lo[0] || tri->bounds[0][0] > hi[0]
				|| tri->bounds[1][1] < lo[1] || tri->bounds[0][1] > hi[1]
				|| tri->bounds[1][2] < lo[2] || tri->bounds[0][2] > hi[2] )
			{
				continue;
			}
		}
		R_ModernDrawWorldSurface( surf );
		drawn++;
	}

	qglEnable( GL_TEXTURE_2D );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();
	p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	VectorCopy( center, modernSunCenter );
	modernSunRange = range;
	modernSunValid = qtrue;
	if ( r_modernDebug->integer == 6 )
	{
		// Relire la carte : un compte de surfaces ne dit pas si quelque chose a
		// vraiment ete ecrit dans la profondeur.
		static float	*peek;
		const int		side = modernSunSize;
		float			mn = 1.0f, mx = 0.0f;
		int				written = -1;

		if ( !peek )
		{
			peek = (float *)malloc( (size_t)side * side * sizeof( float ) );
		}
		if ( peek )
		{
			qglBindTexture( GL_TEXTURE_2D, modernSunTex );
			qglGetTexImage( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, peek );
			qglBindTexture( GL_TEXTURE_2D, 0 );
			written = 0;
			for ( int k = 0; k < side * side; k += 37 )
			{
				if ( peek[k] < mn ) mn = peek[k];
				if ( peek[k] > mx ) mx = peek[k];
				if ( peek[k] < 0.999f ) written++;
			}
			written = (int)( written * 3700.0 / ( (double)side * side ) );
		}
		// Ou le centre lui-meme tombe-t-il dans la carte ? On l'attend vers
		// (0.5, 0.5) : sinon la matrice est fausse et la lecture ne trouvera
		// jamais rien.
		const float cx = modernSunMatrix[0] * center[0] + modernSunMatrix[4] * center[1] + modernSunMatrix[8]  * center[2] + modernSunMatrix[12];
		const float cy = modernSunMatrix[1] * center[0] + modernSunMatrix[5] * center[1] + modernSunMatrix[9]  * center[2] + modernSunMatrix[13];
		const float cz = modernSunMatrix[2] * center[0] + modernSunMatrix[6] * center[1] + modernSunMatrix[10] * center[2] + modernSunMatrix[14];

		ri.Printf( PRINT_ALL, "carte d'ombre: %.0f %.0f %.0f, %i surfaces, profondeur %.4f..%.4f, %i%% ecrit, centre -> %.3f %.3f %.3f\n",
			center[0], center[1], center[2], drawn, mn, mx, written, cx, cy, cz );
	}
}

static void R_ModernPostProcess( void );

void R_ModernFullscreenQuad( void )
{
	qglBegin( GL_QUADS );
		qglTexCoord2f( 0.0f, 0.0f );	qglVertex2f( -1.0f, -1.0f );
		qglTexCoord2f( 1.0f, 0.0f );	qglVertex2f(  1.0f, -1.0f );
		qglTexCoord2f( 1.0f, 1.0f );	qglVertex2f(  1.0f,  1.0f );
		qglTexCoord2f( 0.0f, 1.0f );	qglVertex2f( -1.0f,  1.0f );
	qglEnd();
}

/*
===============
R_ModernPostProcess

Appelee depuis RB_DrawSurfs, une fois le monde dessine et AVANT le 2D : le HUD
et les menus ne doivent jamais passer dans les passes d'image.

On capture ce que le pipeline d'origine vient de peindre (couleur et
profondeur), puis on repeint l'ecran depuis cette capture. A l'etape 0 la passe
ne fait que recopier : l'image doit etre identique a celle d'avant.
===============
*/
static qboolean	modernPending;		// une vue 3D a ete dessinee, la passe lui est due
static qboolean	modernOpaqueDone;	// ... ou la passe a deja eu lieu au milieu de cette vue
static qboolean	modernLatePending;	// la passe tardive du trace (verre, eau, lave) est due au premier 2D
static modernRTParams_t	modernLateParams;

// La frontiere opaque -> transparent de la vue principale : le decor et les
// personnages sont peints, les lames de sabre, lueurs, sprites et effets
// additifs ne le sont pas encore. C'est ICI que la passe doit s'appliquer,
// pour que l'ombre et l'occlusion ne touchent pas ce qui brille (JD : la lame
// du sabre s'eteignait dans l'ombre). La profondeur n'a alors que l'opaque.
void R_ModernOpaqueDone( void )
{
	if ( !r_modern || !r_modern->integer || modernFailed )
	{
		return;
	}
	if ( ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL ) ) || backEnd.viewParms.isPortal )
	{
		return;		// pas le ciel en portail, pas un miroir : seulement la vue principale
	}
	modernOpaqueDone = qtrue;
	modernPending = qfalse;		// une vue precedente (ciel en portail) n'a plus rien a reclamer
	if ( r_modernDebug->integer == 6 )
	{
		ri.Printf( PRINT_ALL, "passe a la frontiere opaque -> transparent (vue %ix%i)\n", backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	}
	R_ModernPostProcess();
}

void R_ModernMarkPending( void )
{
	if ( modernOpaqueDone )
	{	// deja fait pour cette vue ; rien a rattraper au passage en 2D
		modernOpaqueDone = qfalse;
		return;
	}
	modernPending = qtrue;
}

void R_ModernFlush( void )
{
	if ( modernLatePending )
	{	// le transparent est peint : verre, eau, lave
		modernLatePending = qfalse;
		R_ModernRTLatePass( &modernLateParams );
	}
	if ( !modernPending )
	{
		return;
	}
	modernPending = qfalse;
	R_ModernPostProcess();
}

static void R_ModernPostProcess( void )
{
	const int	w = backEnd.viewParms.viewportWidth;
	const int	h = backEnd.viewParms.viewportHeight;
	const int	x = backEnd.viewParms.viewportX;
	const int	y = backEnd.viewParms.viewportY;

	if ( !r_modern->integer || modernFailed )
	{
		return;		// eteint : on ne touche a rien, l'image est celle d'origine
	}
	if ( !modernReady )
	{	// premiere image depuis l'allumage : c'est ici qu'on paie la creation
		R_ModernInit();
		if ( !modernReady )
		{
			return;
		}
	}
	if ( w <= 0 || h <= 0 )
	{
		return;
	}
	if ( w != modernWidth || h != modernHeight )
	{	// la fenetre a change de taille (ou une vue partielle : miroir, portail)
		if ( !R_ModernCreateTargets( w, h ) )
		{
			modernReady = qfalse;
			return;
		}
	}

	// --- capture de ce que le pipeline d'origine vient de peindre
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, modernSceneTex );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, w, h );
	qglBindTexture( GL_TEXTURE_2D, modernDepthTex );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, w, h );

	// --- etat commun aux passes : un quad plein ecran, rien d'autre
	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();
	GL_State( GLS_DEPTHTEST_DISABLE );
	GL_Cull( CT_TWO_SIDED );	// suivi par le moteur : il reactivera lui-meme

	// Les six termes utiles de la projection du moteur : de quoi remonter de la
	// profondeur a une position dans l'espace de la camera.
	const float	*P = backEnd.viewParms.projectionMatrix;
	const float	texelX = 1.0f / (float)w;
	const float	texelY = 1.0f / (float)h;
	const qboolean	wantAo = (qboolean)( r_modernAO->integer && r_modernAOIntensity->value > 0.0f );

	// Le soleil de la carte, amene dans l'espace de la camera. Seule la rotation
	// compte : c'est une direction, pas un point.
	const float	*M = backEnd.viewParms.world.modelMatrix;
	const qboolean	mapHasSun = (qboolean)( tr.sunLight[0] != 0.0f || tr.sunLight[1] != 0.0f || tr.sunLight[2] != 0.0f );
	// AUCUNE carte du jeu d'origine ne declare de soleil (verifie : q3map_sun
	// n'apparait que dans deux shaders d'essai). Exiger une declaration revenait
	// donc a n'avoir d'ombres nulle part. On se sert de la direction de secours
	// du moteur, qui est faite pour ca ; r_modernSun 0 coupe tout.
	const qboolean	wantSun = (qboolean)( r_modernSunStrength->value > 0.0f && r_modernSun->integer );
	vec3_t		sunView;
	vec2_t		sunScreen = { 0.0f, 0.0f };
	qboolean	sunOnScreen = qfalse;

	sunView[0] = M[0] * tr.sunDirection[0] + M[4] * tr.sunDirection[1] + M[8]  * tr.sunDirection[2];
	sunView[1] = M[1] * tr.sunDirection[0] + M[5] * tr.sunDirection[1] + M[9]  * tr.sunDirection[2];
	sunView[2] = M[2] * tr.sunDirection[0] + M[6] * tr.sunDirection[1] + M[10] * tr.sunDirection[2];
	VectorNormalize( sunView );
	// Le soleil a l'ecran : une direction vue comme un point a l'infini. En
	// espace camera on regarde vers les z negatifs, donc un soleil derriere
	// nous (z >= 0) n'a pas de position d'ecran.
	if ( sunView[2] < -0.05f )
	{
		const float ndcX = P[0] * ( sunView[0] / -sunView[2] ) - P[8];
		const float ndcY = P[5] * ( sunView[1] / -sunView[2] ) - P[9];

		sunScreen[0] = ndcX * 0.5f + 0.5f;
		sunScreen[1] = ndcY * 0.5f + 0.5f;
		// on le laisse deborder un peu du cadre : un soleil juste hors champ
		// jette encore ses rais dans l'image
		sunOnScreen = (qboolean)( sunScreen[0] > -0.4f && sunScreen[0] < 1.4f
			&& sunScreen[1] > -0.4f && sunScreen[1] < 1.4f );
	}

	// --- la carte d'ombre : le decor et le soleil sont fixes, on ne la refait
	// que lorsque le joueur s'est eloigne du centre autour duquel elle a ete
	// construite (ou qu'on change sa portee).
	// Le ray tracing prend la place de la carte d'ombre et de l'occlusion en
	// espace ecran quand il est actif et qu'il a pu se mettre en place.
	qboolean		rtDone = qfalse;

	if ( R_ModernRTActive() )
	{
		modernRTParams_t	prm;

		prm.w = w;
		prm.h = h;
		prm.depthTex = modernDepthTex;
		prm.sceneTex = modernSceneTex;
		prm.aoTex = modernAoTex;
		prm.proj = P;
		prm.view = backEnd.viewParms.world.modelMatrix;
		R_ModernInvertView( backEnd.viewParms.world.modelMatrix, prm.invView );
		VectorCopy( tr.sunDirection, prm.sunDir );
		VectorNormalize( prm.sunDir );
		prm.wantSun = wantSun;
		prm.wantAo = wantAo;
		rtDone = R_ModernRTPass( &prm );
		if ( rtDone )
		{
			modernLateParams = prm;
			modernLatePending = qtrue;
		}
		qglViewport( x, y, w, h );
		qglScissor( x, y, w, h );
		modernSunValid = qfalse;
	}

	const qboolean	wantMap = (qboolean)( !rtDone && wantSun && r_modernSunMap->integer && tr.world != NULL );

	if ( wantMap )
	{
		const float	range = r_modernSunMapRange->value > 64.0f ? r_modernSunMapRange->value : 64.0f;
		const float	*org = backEnd.viewParms.ori.origin;
		vec3_t		away;

		VectorSubtract( org, modernSunCenter, away );
		if ( !modernSunValid || modernSunRange != range || VectorLength( away ) > range * 0.35f )
		{
			R_ModernBuildSunMap( tr.sunDirection, org, range );
			qglViewport( x, y, w, h );
			qglScissor( x, y, w, h );
		}
	}
	else
	{
		modernSunValid = qfalse;
	}

	// --- passe 1 : l'occlusion et l'ombre du soleil, dans la meme cible
	if ( !rtDone && ( wantAo || wantSun ) )
	{
		p_glBindFramebuffer( GL_FRAMEBUFFER, modernAoFbo );
		qglViewport( 0, 0, w, h );

		GL_SelectTexture( 0 );
		qglBindTexture( GL_TEXTURE_2D, modernDepthTex );

		p_glUseProgram( modernAoProg );
		p_glUniform1i( p_glGetUniformLocation( modernAoProg, "sceneDepth" ), 0 );
		p_glUniform2f( p_glGetUniformLocation( modernAoProg, "projAB" ), P[0], P[5] );
		p_glUniform2f( p_glGetUniformLocation( modernAoProg, "projCD" ), P[8], P[9] );
		p_glUniform2f( p_glGetUniformLocation( modernAoProg, "projEF" ), P[10], P[14] );
		p_glUniform2f( p_glGetUniformLocation( modernAoProg, "texel" ), texelX, texelY );
		p_glUniform1f( p_glGetUniformLocation( modernAoProg, "radius" ),
			wantAo ? r_modernAORadius->value : 0.0f );
		p_glUniform1f( p_glGetUniformLocation( modernAoProg, "bias" ), 0.08f );
		p_glUniform3f( p_glGetUniformLocation( modernAoProg, "sunDir" ), sunView[0], sunView[1], sunView[2] );
		p_glUniform1f( p_glGetUniformLocation( modernAoProg, "sunOn" ), wantSun ? 1.0f : 0.0f );
		p_glUniform1i( p_glGetUniformLocation( modernAoProg, "dbg" ), r_modernDebug->integer );
		p_glUniform1f( p_glGetUniformLocation( modernAoProg, "sunLength" ), r_modernSunLength->value );
		if ( modernSunValid )
		{	// de l'espace camera a la carte d'ombre, en une seule matrice
			float	invView[16], viewToSun[16];

			R_ModernInvertView( backEnd.viewParms.world.modelMatrix, invView );
			R_ModernMulMat( modernSunMatrix, invView, viewToSun );
			GL_SelectTexture( 1 );
			qglBindTexture( GL_TEXTURE_2D, modernSunTex );
			GL_SelectTexture( 0 );
			p_glUniform1i( p_glGetUniformLocation( modernAoProg, "sunMap" ), 1 );
			p_glUniformMatrix4fv( p_glGetUniformLocation( modernAoProg, "viewToSun" ), 1, GL_FALSE, viewToSun );
			p_glUniform1f( p_glGetUniformLocation( modernAoProg, "sunTexel" ), 1.0f / (float)modernSunSize );
		}
		p_glUniform1f( p_glGetUniformLocation( modernAoProg, "sunMapOn" ), modernSunValid ? 1.0f : 0.0f );
		R_ModernFullscreenQuad();
		if ( modernSunValid )
		{
			GL_SelectTexture( 1 );
			qglBindTexture( GL_TEXTURE_2D, 0 );
			GL_SelectTexture( 0 );
		}

		p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( x, y, w, h );
	}

	// --- passe 2 : composition a l'ecran
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, rtDone ? R_ModernRTAoTex() : modernAoTex );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, modernDepthTex );
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, modernSceneTex );

	if ( rtDone )
	{	// les unites 3 et 4 ne sont pas suivies par le moteur : en direct, et
		// on revient sur la 0 qu'il croit active
		qglActiveTextureARB( GL_TEXTURE3_ARB );
		qglBindTexture( GL_TEXTURE_2D, R_ModernRTLightTex() );
		qglActiveTextureARB( 0x84C4 /* GL_TEXTURE4_ARB */ );
		qglBindTexture( GL_TEXTURE_2D, R_ModernRTReflTex() );
		qglActiveTextureARB( 0x84C5 /* GL_TEXTURE5_ARB */ );
		qglBindTexture( GL_TEXTURE_2D, R_ModernRTMatTex() );
		qglActiveTextureARB( 0x84C6 /* GL_TEXTURE6_ARB */ );
		qglBindTexture( GL_TEXTURE_2D, R_ModernRTMatDepth() );
		qglActiveTextureARB( 0x84C7 /* GL_TEXTURE7_ARB */ );
		qglBindTexture( GL_TEXTURE_2D, R_ModernRTMatGeo() );
		qglActiveTextureARB( GL_TEXTURE0_ARB );
	}
	if ( r_modernDebug->integer == 6 )
	{	// SONDE : les liaisons au moment de la composition
		static int lastSay;

		if ( abs( (int)( backEnd.refdef.time - lastSay ) ) > 1000 )
		{
			GLint	b[7];

			lastSay = backEnd.refdef.time;
			for ( int u = 0; u < 7; u++ )
			{
				qglActiveTextureARB( GL_TEXTURE0_ARB + u );
				qglGetIntegerv( GL_TEXTURE_BINDING_2D, &b[u] );
			}
			qglActiveTextureARB( GL_TEXTURE0_ARB );
			ri.Printf( PRINT_ALL, "composition: unites %i %i %i %i %i %i %i | scene %i prof %i ao %i aoRT %i lum %i refl %i mat %i matprof %i tmu %i prog %i\n",
				b[0], b[1], b[2], b[3], b[4], b[5], b[6], modernSceneTex, modernDepthTex, modernAoTex, R_ModernRTAoTex(),
				R_ModernRTLightTex(), R_ModernRTReflTex(), R_ModernRTMatTex(), R_ModernRTMatDepth(), glState.currenttmu, modernComposite );
		}
	}
	p_glUseProgram( modernComposite );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "rtMat" ), 5 );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "rtMatDepth" ), 6 );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "rtMatGeo" ), 7 );
	p_glUniform3f( p_glGetUniformLocation( modernComposite, "sunView" ), sunView[0], sunView[1], sunView[2] );
	p_glUniform1f( p_glGetUniformLocation( modernComposite, "reliefOn" ), ( rtDone && r_modernRTNormals->integer ) ? r_modernRTNormalStrength->value : 0.0f );
	p_glUniform2f( p_glGetUniformLocation( modernComposite, "projAB" ), P[0], P[5] );
	p_glUniform2f( p_glGetUniformLocation( modernComposite, "projCD" ), P[8], P[9] );
	p_glUniform2f( p_glGetUniformLocation( modernComposite, "projEF" ), P[10], P[14] );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "rtLight" ), 3 );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "rtRefl" ), 4 );
	p_glUniform1f( p_glGetUniformLocation( modernComposite, "rtOn" ), rtDone ? 1.0f : 0.0f );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "scene" ), 0 );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "sceneDepth" ), 1 );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "ao" ), 2 );
	p_glUniform2f( p_glGetUniformLocation( modernComposite, "texel" ), texelX, texelY );
	p_glUniform1f( p_glGetUniformLocation( modernComposite, "intensity" ),
		wantAo ? r_modernAOIntensity->value : 0.0f );
	p_glUniform1f( p_glGetUniformLocation( modernComposite, "sunStrength" ),
		wantSun ? r_modernSunStrength->value : 0.0f );
	{	// les rayons ne valent que si le soleil est dans le cadre
		const qboolean	wantRays = (qboolean)( r_modernRays->integer && sunOnScreen
			&& r_modernRaysStrength->value > 0.0f && r_modernSun->integer );
		vec3_t			col = { 1.0f, 0.95f, 0.85f };

		if ( mapHasSun )
		{	// la teinte du soleil de la carte, ramenee a une echelle raisonnable
			const float m = ( tr.sunLight[0] > tr.sunLight[1] ? tr.sunLight[0] : tr.sunLight[1] );
			const float mx = ( m > tr.sunLight[2] ? m : tr.sunLight[2] );

			if ( mx > 0.01f )
			{
				VectorScale( tr.sunLight, 1.0f / mx, col );
			}
		}
		p_glUniform2f( p_glGetUniformLocation( modernComposite, "sunUV" ), sunScreen[0], sunScreen[1] );
		p_glUniform1f( p_glGetUniformLocation( modernComposite, "raysOn" ),
			wantRays ? r_modernRaysStrength->value : 0.0f );
		p_glUniform3f( p_glGetUniformLocation( modernComposite, "raysColor" ), col[0], col[1], col[2] );
		if ( r_modernDebug->integer == 5 )
		{	// dire ce qu'on calcule plutot que de deviner pourquoi l'ecran est noir
			static int lastSay;

			if ( abs( (int)( backEnd.refdef.time - lastSay ) ) > 1000 )
			{
				lastSay = backEnd.refdef.time;
				ri.Printf( PRINT_ALL, "rayons: soleil vue %.2f %.2f %.2f -> ecran %.2f %.2f, dans le cadre %i, actifs %i, carte a un soleil %i\n",
					sunView[0], sunView[1], sunView[2], sunScreen[0], sunScreen[1],
					(int)sunOnScreen, (int)wantRays, (int)mapHasSun );
			}
		}
	}
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "debugMode" ), r_modernDebug->integer );
	R_ModernFullscreenQuad();

	p_glUseProgram( 0 );

	if ( rtDone )
	{
		qglActiveTextureARB( 0x84C7 );
		qglBindTexture( GL_TEXTURE_2D, 0 );
		qglActiveTextureARB( 0x84C6 );
		qglBindTexture( GL_TEXTURE_2D, 0 );
		qglActiveTextureARB( 0x84C5 );
		qglBindTexture( GL_TEXTURE_2D, 0 );
		qglActiveTextureARB( 0x84C4 /* GL_TEXTURE4_ARB */ );
		qglBindTexture( GL_TEXTURE_2D, 0 );
		qglActiveTextureARB( GL_TEXTURE3_ARB );
		qglBindTexture( GL_TEXTURE_2D, 0 );
		qglActiveTextureARB( GL_TEXTURE0_ARB );
	}

	// rendre les unites de texture telles qu'on les a trouvees : le pipeline
	// d'origine ne s'attend pas a les voir occupees
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	// le cache de liaisons du moteur ne sait rien de tout ca : l'invalider,
	// sinon son prochain GL_Bind de la meme image serait saute (on tourne
	// maintenant AU MILIEU de la liste des surfaces, plus en fin d'image)
	glState.currenttextures[0] = glState.currenttextures[1] = -1;

	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();

	if ( r_modernDebug->integer < 0 )
	{	// -1 : signaler toute erreur GL laissee par la passe
		const GLenum err = qglGetError();

		if ( err != GL_NO_ERROR )
		{
			ri.Printf( PRINT_ALL, "rendu moderne : erreur GL 0x%x\n", err );
		}
	}
}
