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

cvar_t	*r_modern;
cvar_t	*r_modernDebug;
cvar_t	*r_modernAO;
cvar_t	*r_modernAOIntensity;
cvar_t	*r_modernAORadius;
cvar_t	*r_modernSun;
cvar_t	*r_modernSunStrength;
cvar_t	*r_modernSunLength;

// ---------------------------------------------------------------- entrees GL
// Le moteur ne connait que le GL 1.x : on va chercher nous-memes ce qu'il faut.
// Les pilotes de bureau donnent deja un contexte de compatibilite recent, donc
// il n'y a en general rien a demander a SDL - on verifie, c'est tout.

typedef void	(APIENTRY *PFN_glGenFramebuffers)( GLsizei, GLuint * );
typedef void	(APIENTRY *PFN_glDeleteFramebuffers)( GLsizei, const GLuint * );
typedef void	(APIENTRY *PFN_glBindFramebuffer)( GLenum, GLuint );
typedef void	(APIENTRY *PFN_glFramebufferTexture2D)( GLenum, GLenum, GLenum, GLuint, GLint );
typedef GLenum	(APIENTRY *PFN_glCheckFramebufferStatus)( GLenum );
typedef GLuint	(APIENTRY *PFN_glCreateShader)( GLenum );
typedef void	(APIENTRY *PFN_glShaderSource)( GLuint, GLsizei, const GLchar *const *, const GLint * );
typedef void	(APIENTRY *PFN_glCompileShader)( GLuint );
typedef void	(APIENTRY *PFN_glGetShaderiv)( GLuint, GLenum, GLint * );
typedef void	(APIENTRY *PFN_glGetShaderInfoLog)( GLuint, GLsizei, GLsizei *, GLchar * );
typedef void	(APIENTRY *PFN_glDeleteShader)( GLuint );
typedef GLuint	(APIENTRY *PFN_glCreateProgram)( void );
typedef void	(APIENTRY *PFN_glAttachShader)( GLuint, GLuint );
typedef void	(APIENTRY *PFN_glLinkProgram)( GLuint );
typedef void	(APIENTRY *PFN_glGetProgramiv)( GLuint, GLenum, GLint * );
typedef void	(APIENTRY *PFN_glGetProgramInfoLog)( GLuint, GLsizei, GLsizei *, GLchar * );
typedef void	(APIENTRY *PFN_glDeleteProgram)( GLuint );
typedef void	(APIENTRY *PFN_glUseProgram)( GLuint );
typedef GLint	(APIENTRY *PFN_glGetUniformLocation)( GLuint, const GLchar * );
typedef void	(APIENTRY *PFN_glUniform1i)( GLint, GLint );
typedef void	(APIENTRY *PFN_glUniform1f)( GLint, GLfloat );
typedef void	(APIENTRY *PFN_glUniform2f)( GLint, GLfloat, GLfloat );
typedef void	(APIENTRY *PFN_glUniform3f)( GLint, GLfloat, GLfloat, GLfloat );
typedef void	(APIENTRY *PFN_glUniform4f)( GLint, GLfloat, GLfloat, GLfloat, GLfloat );
typedef void	(APIENTRY *PFN_glUniformMatrix4fv)( GLint, GLsizei, GLboolean, const GLfloat * );

static PFN_glGenFramebuffers		p_glGenFramebuffers;
static PFN_glDeleteFramebuffers		p_glDeleteFramebuffers;
static PFN_glBindFramebuffer		p_glBindFramebuffer;
static PFN_glFramebufferTexture2D	p_glFramebufferTexture2D;
static PFN_glCheckFramebufferStatus	p_glCheckFramebufferStatus;
static PFN_glCreateShader			p_glCreateShader;
static PFN_glShaderSource			p_glShaderSource;
static PFN_glCompileShader			p_glCompileShader;
static PFN_glGetShaderiv			p_glGetShaderiv;
static PFN_glGetShaderInfoLog		p_glGetShaderInfoLog;
static PFN_glDeleteShader			p_glDeleteShader;
static PFN_glCreateProgram			p_glCreateProgram;
static PFN_glAttachShader			p_glAttachShader;
static PFN_glLinkProgram			p_glLinkProgram;
static PFN_glGetProgramiv			p_glGetProgramiv;
static PFN_glGetProgramInfoLog		p_glGetProgramInfoLog;
static PFN_glDeleteProgram			p_glDeleteProgram;
static PFN_glUseProgram				p_glUseProgram;
static PFN_glGetUniformLocation		p_glGetUniformLocation;
static PFN_glUniform1i				p_glUniform1i;
static PFN_glUniform1f				p_glUniform1f;
static PFN_glUniform2f				p_glUniform2f;
static PFN_glUniform3f				p_glUniform3f;
static PFN_glUniform4f				p_glUniform4f;
static PFN_glUniformMatrix4fv		p_glUniformMatrix4fv;

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

static GLuint R_ModernCompile( const char *vertex, const char *fragment, const char *what )
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
	"const int SUN_STEPS = 24;\n"
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
	"	gl_FragColor = vec4( clamp( 1.0 - occ, 0.0, 1.0 ), shade, 0.0, 1.0 );\n"
	"}\n";

// Composition : l'occlusion est bruitee par construction (un angle aleatoire
// par pixel), on l'adoucit ici au lieu d'y consacrer une passe de plus.
static const char *fsComposite =
	"#version 120\n"
	"uniform sampler2D scene;\n"
	"uniform sampler2D sceneDepth;\n"
	"uniform sampler2D ao;\n"
	"uniform vec2 texel;\n"
	"uniform float intensity;\n"
	"uniform float sunStrength;\n"
	"uniform int debugMode;\n"
	"varying vec2 uv;\n"
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
	"	sh = mix( 1.0, sh / 25.0, sunStrength );\n"
	"	if ( debugMode == 1 ) {\n"
	"		float d = texture2D( sceneDepth, uv ).r;\n"
	"		gl_FragColor = vec4( vec3( 1.0 - pow( d, 64.0 ) ), 1.0 );\n"
	"	} else if ( debugMode == 3 ) {\n"
	"		gl_FragColor = vec4( vec3( a ), 1.0 );\n"		// l'occlusion seule
	"	} else if ( debugMode == 2 && uv.x > 0.5 ) {\n"
	"		gl_FragColor = vec4( c.rgb * vec3( 1.0, 0.45, 0.25 ), c.a );\n"
	"	} else if ( debugMode == 4 ) {\n"
	"		gl_FragColor = vec4( vec3( sh ), 1.0 );\n"		// l'ombre seule
	"	} else {\n"
	"		gl_FragColor = vec4( c.rgb * a * sh, c.a );\n"
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
static void R_ModernDrainErrors( void )
{
	for ( int i = 0; i < 16 && qglGetError() != GL_NO_ERROR; i++ ) {}
}

static qboolean R_ModernStep( const char *what )
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
	modernAoProg = R_ModernCompile( vsPassthrough, va( fsAo, glslViewPos ), "occlusion ambiante" );
	modernComposite = R_ModernCompile( vsPassthrough, fsComposite, "composition" );
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
	if ( modernSceneTex )
	{
		qglDeleteTextures( 1, &modernSceneTex );
		qglDeleteTextures( 1, &modernDepthTex );
		qglDeleteTextures( 1, &modernAoTex );
		modernSceneTex = modernDepthTex = modernAoTex = 0;
	}
	modernReady = qfalse;
}

static void R_ModernPostProcess( void );

static void R_ModernFullscreenQuad( void )
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

void R_ModernMarkPending( void )
{
	modernPending = qtrue;
}

void R_ModernFlush( void )
{
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
	qglDisable( GL_CULL_FACE );

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
	const qboolean	wantSun = (qboolean)( r_modernSunStrength->value > 0.0f
		&& ( r_modernSun->integer >= 2 || ( r_modernSun->integer && mapHasSun ) ) );
	vec3_t		sunView;

	sunView[0] = M[0] * tr.sunDirection[0] + M[4] * tr.sunDirection[1] + M[8]  * tr.sunDirection[2];
	sunView[1] = M[1] * tr.sunDirection[0] + M[5] * tr.sunDirection[1] + M[9]  * tr.sunDirection[2];
	sunView[2] = M[2] * tr.sunDirection[0] + M[6] * tr.sunDirection[1] + M[10] * tr.sunDirection[2];
	VectorNormalize( sunView );

	// --- passe 1 : l'occlusion et l'ombre du soleil, dans la meme cible
	if ( wantAo || wantSun )
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
		p_glUniform1f( p_glGetUniformLocation( modernAoProg, "sunLength" ), r_modernSunLength->value );
		R_ModernFullscreenQuad();

		p_glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( x, y, w, h );
	}

	// --- passe 2 : composition a l'ecran
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, modernAoTex );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, modernDepthTex );
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, modernSceneTex );

	p_glUseProgram( modernComposite );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "scene" ), 0 );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "sceneDepth" ), 1 );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "ao" ), 2 );
	p_glUniform2f( p_glGetUniformLocation( modernComposite, "texel" ), texelX, texelY );
	p_glUniform1f( p_glGetUniformLocation( modernComposite, "intensity" ),
		wantAo ? r_modernAOIntensity->value : 0.0f );
	p_glUniform1f( p_glGetUniformLocation( modernComposite, "sunStrength" ),
		wantSun ? r_modernSunStrength->value : 0.0f );
	p_glUniform1i( p_glGetUniformLocation( modernComposite, "debugMode" ), r_modernDebug->integer );
	R_ModernFullscreenQuad();

	p_glUseProgram( 0 );

	// rendre les unites de texture telles qu'on les a trouvees : le pipeline
	// d'origine ne s'attend pas a les voir occupees
	GL_SelectTexture( 2 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	GL_SelectTexture( 0 );

	qglEnable( GL_CULL_FACE );
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
