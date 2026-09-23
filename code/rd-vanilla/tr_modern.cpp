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
static GLuint	modernPassthrough;	// programme de base (etape 0)

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

static const char *fsPassthrough =
	"#version 120\n"
	"uniform sampler2D scene;\n"
	"uniform sampler2D sceneDepth;\n"
	"uniform int debugMode;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"	vec4 c = texture2D( scene, uv );\n"
	"	if ( debugMode == 1 ) {\n"
	"		// la profondeur brute est ecrasee contre 1.0 : une puissance elevee\n"
	"		// etale ce qui nous interesse, le proche\n"
	"		float d = texture2D( sceneDepth, uv ).r;\n"
	"		float v = pow( d, 64.0 );\n"
	"		c = vec4( vec3( 1.0 - v ), 1.0 );\n"
	"	} else if ( debugMode == 2 && uv.x > 0.5 ) {\n"
	"		c.rgb = c.rgb * vec3( 1.0, 0.45, 0.25 );\n"
	"	}\n"
	"	gl_FragColor = c;\n"
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
		modernSceneTex = modernDepthTex = 0;
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

	qglBindTexture( GL_TEXTURE_2D, 0 );

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
	modernPassthrough = R_ModernCompile( vsPassthrough, fsPassthrough, "passe de base" );
	if ( !modernPassthrough )
	{
		modernFailed = qtrue;
		return;
	}
	modernReady = qtrue;
	ri.Printf( PRINT_ALL, "...actif en %ix%i\n", modernWidth, modernHeight );
}

void R_ModernShutdown( void )
{
	if ( modernPassthrough )
	{
		p_glDeleteProgram( modernPassthrough );
		modernPassthrough = 0;
	}
	if ( modernSceneTex )
	{
		qglDeleteTextures( 1, &modernSceneTex );
		qglDeleteTextures( 1, &modernDepthTex );
		modernSceneTex = modernDepthTex = 0;
	}
	modernReady = qfalse;
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
void R_ModernPostProcess( void )
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

	// --- capture
	qglBindTexture( GL_TEXTURE_2D, modernSceneTex );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, w, h );
	qglBindTexture( GL_TEXTURE_2D, modernDepthTex );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, w, h );

	// --- etat minimal pour un quad plein ecran, en coordonnees normalisees
	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();

	GL_State( GLS_DEPTHTEST_DISABLE );
	qglDisable( GL_CULL_FACE );

	GL_SelectTexture( 1 );
	qglBindTexture( GL_TEXTURE_2D, modernDepthTex );
	GL_SelectTexture( 0 );
	qglBindTexture( GL_TEXTURE_2D, modernSceneTex );

	p_glUseProgram( modernPassthrough );
	p_glUniform1i( p_glGetUniformLocation( modernPassthrough, "scene" ), 0 );
	p_glUniform1i( p_glGetUniformLocation( modernPassthrough, "sceneDepth" ), 1 );
	p_glUniform1i( p_glGetUniformLocation( modernPassthrough, "debugMode" ), r_modernDebug->integer );

	qglBegin( GL_QUADS );
		qglTexCoord2f( 0.0f, 0.0f );	qglVertex2f( -1.0f, -1.0f );
		qglTexCoord2f( 1.0f, 0.0f );	qglVertex2f(  1.0f, -1.0f );
		qglTexCoord2f( 1.0f, 1.0f );	qglVertex2f(  1.0f,  1.0f );
		qglTexCoord2f( 0.0f, 1.0f );	qglVertex2f( -1.0f,  1.0f );
	qglEnd();

	p_glUseProgram( 0 );

	// rendre les unites de texture telles qu'on les a trouvees : le pipeline
	// d'origine ne s'attend pas a voir l'unite 1 occupee
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
