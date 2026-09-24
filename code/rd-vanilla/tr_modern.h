/*
===========================================================================
JACoop - la couche de rendu moderne : ce que tr_modern.cpp partage avec
tr_modern_rt.cpp (le ray tracing). Les points d'entree GL recuperes a la main
(le moteur d'origine ne connait pas les shaders GLSL), les petits outils, et
le contrat de la passe tracee.
===========================================================================
*/
#pragma once

#include "tr_local.h"

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

extern PFN_glGenFramebuffers		p_glGenFramebuffers;
extern PFN_glDeleteFramebuffers		p_glDeleteFramebuffers;
extern PFN_glBindFramebuffer		p_glBindFramebuffer;
extern PFN_glFramebufferTexture2D	p_glFramebufferTexture2D;
extern PFN_glCheckFramebufferStatus	p_glCheckFramebufferStatus;
extern PFN_glCreateShader			p_glCreateShader;
extern PFN_glShaderSource			p_glShaderSource;
extern PFN_glCompileShader			p_glCompileShader;
extern PFN_glGetShaderiv			p_glGetShaderiv;
extern PFN_glGetShaderInfoLog		p_glGetShaderInfoLog;
extern PFN_glDeleteShader			p_glDeleteShader;
extern PFN_glCreateProgram			p_glCreateProgram;
extern PFN_glAttachShader			p_glAttachShader;
extern PFN_glLinkProgram			p_glLinkProgram;
extern PFN_glGetProgramiv			p_glGetProgramiv;
extern PFN_glGetProgramInfoLog		p_glGetProgramInfoLog;
extern PFN_glDeleteProgram			p_glDeleteProgram;
extern PFN_glUseProgram				p_glUseProgram;
extern PFN_glGetUniformLocation		p_glGetUniformLocation;
extern PFN_glUniform1i				p_glUniform1i;
extern PFN_glUniform1f				p_glUniform1f;
extern PFN_glUniform2f				p_glUniform2f;
extern PFN_glUniform3f				p_glUniform3f;
extern PFN_glUniform4f				p_glUniform4f;
extern PFN_glUniformMatrix4fv		p_glUniformMatrix4fv;

// les outils de tr_modern.cpp
GLuint		R_ModernCompile( const char *vertex, const char *fragment, const char *what );
qboolean	R_ModernStep( const char *what );
void		R_ModernDrainErrors( void );
void		R_ModernFullscreenQuad( void );

// --- le ray tracing (tr_modern_rt.cpp) ------------------------------------
// Ce que la passe tracee a besoin de savoir de la vue en cours.
typedef struct modernRTParams_s {
	int			w, h;
	GLuint		depthTex;		// la profondeur de la scene (opaque seul)
	GLuint		sceneTex;		// sa couleur
	GLuint		aoTex;			// la cible R = occlusion, G = ombre (lue par la composition)
	const float	*proj;			// la projection du moteur
	const float	*view;			// monde -> camera
	float		invView[16];	// camera -> monde
	vec3_t		sunDir;			// vers le soleil, dans le monde
	qboolean	wantSun;
	qboolean	wantAo;
} modernRTParams_t;

qboolean	R_ModernRTPass( const modernRTParams_t *p );	// qfalse : pas fait, retomber sur la voie classique
GLuint		R_ModernRTLightTex( void );		// lumiere ajoutee (rgb)
GLuint		R_ModernRTReflTex( void );		// reflet (rgb) et sa force (a)
GLuint		R_ModernRTAoTex( void );		// occlusion (r) et ombre (g), a l'echelle de la passe
